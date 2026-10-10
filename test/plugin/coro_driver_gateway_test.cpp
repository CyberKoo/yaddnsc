//
// Coroutine driver gateway contract tests.
//
// These drive the real dlopen'ed whiteboard test plugin through the coroutine
// gateway: the worker-side ABI cycle, concurrent cycles of one driver, the
// worker ↔ loop HTTP bridge against an in-process server, the exception
// firewall and shutdown cancellation. Timing margins are generous so the
// assertions do not depend on machine speed.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; coroutine bodies use EXPECT_* only.
//

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "domain/error/error.h"
#include "infrastructure/coro/coro.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "infrastructure/plugin/shared_library.h"
#include "mocks/null_logger.h"
#include "support/fmt.hpp"
#include "support/util/fd.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::string_view PLUGIN_PATH = TEST_PLUGIN_PATH;
const std::string DRIVER_NAME = "test_driver_plugin";

/// The whiteboard plugin's test-only counters, read through its extra exports.
struct PluginState {
    std::uint64_t creates = 0;
    std::uint64_t updates = 0;
    std::uint64_t destroys = 0;
    std::uint64_t create_seq = 0;
    std::uint64_t update_seq = 0;
    std::uint64_t destroy_seq = 0;
};

/// Opens the plugin module a second time to reach the test-only exports. The
/// shared object is loaded once, so these read the same globals the catalog
/// drives.
class PluginControl {
public:
    explicit PluginControl(const std::string& path) {
        auto library = SharedLibrary::open(path);
        if (!library.has_value()) {
            return;
        }
        library_ = std::move(*library);
        reset_ = reinterpret_cast<void (*)()>(library_.resolve("test_plugin_reset_state"));  // NOLINT
        get_ = reinterpret_cast<GetState>(library_.resolve("test_plugin_get_state"));        // NOLINT
    }

    void reset() const {
        if (reset_ != nullptr) {
            reset_();
        }
    }

    [[nodiscard]] PluginState state() const {
        PluginState value;
        if (get_ != nullptr) {
            get_(&value.creates, &value.updates, &value.destroys, &value.create_seq, &value.update_seq,
                 &value.destroy_seq);
        }
        return value;
    }

private:
    using GetState = void (*)(std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*,
                              std::uint64_t*);

    SharedLibrary library_;
    void (*reset_)() = nullptr;
    GetState get_ = nullptr;
};

/// In-process HTTP/1.1 server: one handler thread per connection. Each request
/// holds the connection for `hold` (or until shutdown) before answering, which
/// lets tests create slow and never-answering exchanges. The optional callback
/// runs as soon as a request is read.
class LoopbackHttpServer {
public:
    using OnRequest = std::function<void(std::uint32_t index)>;

    explicit LoopbackHttpServer(std::chrono::milliseconds hold, OnRequest on_request = {})
        : hold_(hold), on_request_(std::move(on_request)) {
        listener_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        const int enabled = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));

        socklen_t length = sizeof(address);
        ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::listen(listener_.get(), 16);
        accept_thread_ = std::jthread([this] { accept_loop(); });
    }

    ~LoopbackHttpServer() {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (listener_) {
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                ::close(wake);
            }
            listener_.reset();
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& handler : handlers_) {
            if (handler.joinable()) {
                handler.join();
            }
        }
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::uint32_t request_count() const noexcept { return requests_.load(); }

private:
    void accept_loop() {
        for (;;) {
            {
                const std::lock_guard lock(mutex_);
                if (stop_) {
                    return;
                }
            }
            const int raw = ::accept(listener_.get(), nullptr, nullptr);
            if (raw < 0) {
                return;
            }
            {
                const std::lock_guard lock(mutex_);
                if (stop_) {
                    ::close(raw);
                    return;
                }
            }
            handlers_.emplace_back([this, raw] { handle(raw); });
        }
    }

    void handle(const int raw) {
        const Utils::UniqueFd connection{raw};
        std::string buffer;
        std::array<char, 1024> chunk{};
        while (buffer.find("\r\n\r\n") == std::string::npos) {
            const ssize_t got = ::recv(connection.get(), chunk.data(), chunk.size(), 0);
            if (got <= 0) {
                return;
            }
            buffer.append(chunk.data(), static_cast<std::size_t>(got));
        }
        const std::uint32_t index = requests_.fetch_add(1);
        if (on_request_) {
            on_request_(index);
        }

        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, hold_, [this] { return stop_; });
        }
        if (stop_) {
            return;
        }

        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        std::size_t sent = 0;
        while (sent < response.size()) {
            const ssize_t wrote = ::send(connection.get(), response.data() + sent, response.size() - sent, 0);
            if (wrote <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    std::chrono::milliseconds hold_;
    OnRequest on_request_;
    Utils::UniqueFd listener_;
    std::uint16_t port_{0};
    std::atomic<std::uint32_t> requests_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::jthread accept_thread_;
    std::vector<std::jthread> handlers_;
};

[[nodiscard]] domain::DriverUpdateCommand make_command(std::string driver_params) {
    return domain::DriverUpdateCommand{
        .driver_params = std::move(driver_params),
        .ip_addr = "192.0.2.1",
        .rd_type = "A",
        .domain = "example.com",
        .subdomain = "www",
        .fqdn = "www.example.com",
    };
}

[[nodiscard]] plugin::DriverGateway::Options default_options(std::chrono::milliseconds budget = 3000ms) {
    plugin::DriverGateway::Options options;
    options.bridge_wait_budget = budget;
    return options;
}

[[nodiscard]] std::string exchange_params(const std::uint16_t port, const std::uint32_t count = 1) {
    return fmt::format(R"({{"op":"exchange","http_count":{},"url":"http://127.0.0.1:{}"}})", count, port);
}

/// Waits until `server` has taken `expected` requests, then cancels `cycle` so
/// the update it owns is abandoned mid-exchange. Returns whether the server got
/// there within `give_up`, so a cycle that never issues its request fails the
/// test instead of hanging it.
///
/// Driving the abandon off the request rather than a fixed delay is what makes
/// it deterministic. Before the request lands the cycle may still be queued in
/// the offload pool, and a cancel then drops that queued job instead of
/// cancelling a running exchange — the behaviour
/// AbandonedQueuedCycle_DoesNotEnterTheDriver covers. A fixed delay short
/// enough to abandon a running cycle is also short enough to lose that race on
/// a cold pool.
[[nodiscard]] coro::Task<bool> abandon_when_requested(const LoopbackHttpServer& server, const std::uint32_t expected,
                                                      coro::TaskGroup& cycle, std::chrono::milliseconds give_up = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + give_up;
    while (server.request_count() < expected && std::chrono::steady_clock::now() < deadline) {
        co_await coro::sleep_for(1ms);
    }
    cycle.cancel();
    co_return server.request_count() >= expected;
}

/// Run a task-factory lambda on `loop` (coro::run takes an already-built Task).
template<typename Fn>
void run_loop(coro::Loop& loop, Fn&& body) {
    coro::run(loop, body());
}

/// Fixture: catalog with the whiteboard plugin and a recording logger.
class CoroDriverGatewayTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_NO_THROW(catalog_.load_driver(std::string(PLUGIN_PATH))); }

    DriverCatalog catalog_;
    NullLogger logger_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Bridge round-trip
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, HttpExchangeRoundTripsThroughTheBridge) {
    LoopbackHttpServer server{0ms};
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;

    const std::string params = exchange_params(server.port(), 2);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options());
            result = co_await gateway.update(DRIVER_NAME, make_command(params));
            co_return;
        });
        co_return;
    });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(server.request_count(), 2u);
}

// ---------------------------------------------------------------------------
// Concurrent cycles
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, ConcurrentCyclesOfOneDriverRunInParallel) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    std::mutex snapshot_mutex;
    std::vector<PluginState> at_request;
    LoopbackHttpServer server{300ms, [&](std::uint32_t) {
                                  const PluginState snapshot = control.state();
                                  const std::lock_guard lock(snapshot_mutex);
                                  at_request.push_back(snapshot);
                              }};

    coro::Loop loop;
    std::expected<void, domain::DriverError> first_result;
    std::expected<void, domain::DriverError> second_result;
    const std::string params = exchange_params(server.port(), 1);

    const auto started = std::chrono::steady_clock::now();
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options());
            auto first = group.spawn(gateway.update(DRIVER_NAME, make_command(params)));
            second_result = co_await gateway.update(DRIVER_NAME, make_command(params));
            first_result = co_await first;
            co_return;
        });
        co_return;
    });
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    ASSERT_TRUE(first_result.has_value()) << first_result.error().message;
    ASSERT_TRUE(second_result.has_value()) << second_result.error().message;
    // Neither cycle waited for the other: both exchanges began while no instance
    // had been destroyed yet. The ABI allows concurrent cycles on distinct
    // instances, and every cycle creates a fresh one.
    ASSERT_EQ(at_request.size(), 2u);
    EXPECT_EQ(at_request[0].destroys, 0u);
    EXPECT_EQ(at_request[1].destroys, 0u);
    // Concurrent cycles finish in about one hold, not two.
    EXPECT_LT(elapsed, 500ms);
    EXPECT_EQ(server.request_count(), 2u);
}

// ---------------------------------------------------------------------------
// Abandon
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, AbandonedCycleCancelsItsInFlightExchange) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    // The server accepts and never answers within the test, so only the
    // abandon's cancellation of the in-flight exchange can release the worker
    // short of the 30s bridge budget.
    LoopbackHttpServer server{30s};
    coro::Loop loop;
    bool saw_request = false;
    const std::string params = exchange_params(server.port(), 1);

    const auto started = std::chrono::steady_clock::now();
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options(30s));
            // A nested group scopes the abandon to this one update; the outer
            // scope, and with it the gateway's bridge, stay live.
            co_await coro::supervisor_group([&](coro::TaskGroup& cycle) -> coro::Task<void> {
                // Not joined: a cancelled child carries no result value, so a
                // join would read an empty optional. The group joins it at
                // scope exit.
                cycle.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                saw_request = co_await abandon_when_requested(server, 1, cycle);
                co_return;
            });
            co_return;
        });
        co_return;
    });
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    // The exchange really was in flight, so the abandon cancelled a running
    // cycle rather than dropping a queued one.
    EXPECT_TRUE(saw_request);
    // The in-flight exchange was cancelled, so the plugin's update returned
    // CANCELLED and the worker's cycle wound down at once instead of waiting
    // out the 30s server hold.
    EXPECT_LT(elapsed, 5s);
    // The worker thread unwinds (plugin update returns, destroy runs) a moment
    // after the loop side is released; wait for it, bounded well under the
    // 30s the un-cancelled exchange would have taken.
    PluginState state;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        state = control.state();
        if (state.destroys == 1u) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(state.creates, 1u);
    EXPECT_EQ(state.updates, 1u);
    EXPECT_EQ(state.destroys, 1u);
    EXPECT_EQ(server.request_count(), 1u);
}

TEST_F(CoroDriverGatewayTest, AbandonedCycleWindsDownAndFreesTheNextUpdate) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    LoopbackHttpServer server{200ms};
    coro::Loop loop;
    std::expected<void, domain::DriverError> second_result;
    bool saw_request = false;
    const std::string params = exchange_params(server.port(), 1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options());
            // A nested group scopes the abandon to this one update; the outer
            // scope, and with it the gateway's bridge, stay live. Spawning the
            // member coroutine directly keeps its frame off GCC's elision
            // path, which would hand spawn() a stack frame to own.
            co_await coro::supervisor_group([&](coro::TaskGroup& cycle) -> coro::Task<void> {
                cycle.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                saw_request = co_await abandon_when_requested(server, 1, cycle);
                co_return;
            });
            // The abandoned cycle's in-flight exchange is cancelled and its
            // wind-down runs on the worker; this update does not wait for it.
            second_result = co_await gateway.update(DRIVER_NAME, make_command(params));
            co_return;
        });
        co_return;
    });

    // The first cycle's exchange was in flight when the abandon landed, so the
    // cancel reached a running cycle rather than dropping a queued one.
    EXPECT_TRUE(saw_request);
    ASSERT_TRUE(second_result.has_value()) << second_result.error().message;

    // Both cycles ran the full create → update → destroy sequence: the
    // abandoned one by returning CANCELLED from its aborted exchange, the
    // second one with a real answer.
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 2u);
    EXPECT_EQ(state.updates, 2u);
    EXPECT_EQ(state.destroys, 2u);
    // The abandoned cycle's request did reach the server before the cancel
    // landed, and the second cycle ran its own exchange.
    EXPECT_EQ(server.request_count(), 2u);
}

TEST_F(CoroDriverGatewayTest, AbandonedQueuedCycle_DoesNotEnterTheDriver) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    // One offload worker: the first cycle's exchange holds it for 200ms, so the
    // second update is still queued on the pool when its 20ms budget abandons it.
    LoopbackHttpServer server{200ms};
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::expected<void, domain::DriverError> first_result;
    bool abandoned_returned = false;
    std::expected<void, domain::DriverError> third_result;
    bool timed_out = false;
    const std::string params = exchange_params(server.port(), 1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options());
            auto first = group.spawn(gateway.update(DRIVER_NAME, make_command(params)));
            // Let the first cycle reach the worker before queueing the second.
            co_await coro::sleep_for(20ms);
            const auto outcome = co_await coro::with_timeout(20ms, [&]() -> coro::Task<void> {
                co_await gateway.update(DRIVER_NAME, make_command(params));
                abandoned_returned = true;
                co_return;
            });
            timed_out = outcome.timed_out;
            // Drains the pool queue behind the cancelled queued job.
            third_result = co_await gateway.update(DRIVER_NAME, make_command(params));
            first_result = co_await first;
            co_return;
        });
        co_return;
    });

    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(abandoned_returned);
    ASSERT_TRUE(first_result.has_value()) << first_result.error().message;
    ASSERT_TRUE(third_result.has_value()) << third_result.error().message;

    // The queued job was cancelled before the single worker could start it, so
    // only the first and the third cycles ever entered the driver.
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 2u);
    EXPECT_EQ(state.updates, 2u);
    EXPECT_EQ(state.destroys, 2u);
    EXPECT_EQ(server.request_count(), 2u);
}

// ---------------------------------------------------------------------------
// Exception firewall
// ---------------------------------------------------------------------------

TEST(CoroDriverGatewayFirewall, EntryPointExceptionsBecomeDriverErrors) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(THROWING_FIXTURE)));
    NullLogger logger;

    coro::Loop loop;
    std::expected<void, domain::DriverError> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog, logger, loop, group, default_options());
            result = co_await gateway.update("throwing", make_command(R"({})"));
            co_return;
        });
        co_return;
    });

    // The plugin's create() throws across the ABI; the module's firewall turns
    // it into an error status, which maps to UNKNOWN — no exception escapes.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
}

// ---------------------------------------------------------------------------
// Shutdown cancellation
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, ShutdownCancelsInFlightBridgeExchange) {
    // The server accepts and never answers, so only cancellation can release
    // the worker short of the 30s bridge budget.
    LoopbackHttpServer server{30s};
    coro::Loop loop;
    bool saw_request = false;
    const std::string params = exchange_params(server.port(), 1);

    const auto started = std::chrono::steady_clock::now();
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options(30s));
            group.spawn(gateway.update(DRIVER_NAME, make_command(params)));
            // Shutdown: cancel the bridge scope once the exchange is in flight.
            // The exchange is cancelled at its await, the promise is fulfilled
            // with a cancellation error, and the worker unblocks instead of
            // waiting out the 30s budget.
            saw_request = co_await abandon_when_requested(server, 1, group);
            co_return;
        });
        co_return;
    });
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    // The request was in flight, so shutdown is what released the worker
    // rather than a cycle that was still queued when the scope came down.
    EXPECT_TRUE(saw_request);
    EXPECT_LT(elapsed, 5s);
}

TEST_F(CoroDriverGatewayTest, PluginCancelledStatus_PropagatesAsControlException) {
    coro::Loop loop;
    bool returned = false;
    bool cancelled = false;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog_, logger_, loop, group, default_options());
            const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope&) -> coro::Task<void> {
                co_await gateway.update(DRIVER_NAME, make_command(R"({"op":"fail","status":"cancelled"})"));
                returned = true;
            });
            cancelled = outcome.cancelled && !outcome.completed;
        });
    });
    EXPECT_FALSE(returned);
    EXPECT_TRUE(cancelled);
}
