//
// Coroutine driver gateway contract tests.
//
// These drive the real dlopen'ed whiteboard test plugin through the coroutine
// gateway: the worker-side ABI cycle, concurrent cycles of one driver, the
// worker ↔ loop HTTP bridge against an in-process server, the exception
// firewall and shutdown cancellation. Cancellation tests hold responses behind
// explicit gates rather than racing a finite response delay.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; coroutine bodies use EXPECT_* only.
//

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <expected>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/coro.h"
#include "coro/detail/nursery_state.hpp"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/plugin/bridge.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "infrastructure/plugin/shared_library.h"
#include "mocks/null_logger.h"
#include "support/fmt.hpp"
#include "support/util/fd.hpp"
#include "yaddnsc/sdk/driver_abi.h"
#include "yaddnsc/util/format.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::string_view PLUGIN_PATH = TEST_PLUGIN_PATH;
constexpr std::string_view ASYNC_DRIVER_PATH = TEST_ASYNC_PLUGIN_PATH;
constexpr std::string_view ASYNC_SDK_PATH = ASYNC_SDK_DRIVER_PATH;
constexpr std::string_view ASYNC_CREATE_FAILS_PATH = ASYNC_CREATE_FAILS_FIXTURE;
constexpr std::string_view ASYNC_SLOW_RESUME_PATH = ASYNC_SLOW_RESUME_FIXTURE;
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
        thread_violations_ =
            reinterpret_cast<std::uint64_t (*)()>(library_.resolve("test_plugin_thread_violations"));  // NOLINT
        validation_started_ =
            reinterpret_cast<bool (*)()>(library_.resolve("test_plugin_validation_started"));  // NOLINT
        release_validation_ =
            reinterpret_cast<void (*)()>(library_.resolve("test_plugin_release_validation"));  // NOLINT
        last_failed_exchange_status_ = reinterpret_cast<yaddnsc_status (*)()>(
            library_.resolve("test_plugin_last_failed_exchange_status"));  // NOLINT
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

    [[nodiscard]] std::uint64_t thread_violations() const { return thread_violations_(); }

    [[nodiscard]] bool validation_started() const { return validation_started_(); }

    void release_validation() const { release_validation_(); }

    [[nodiscard]] yaddnsc_status last_failed_exchange_status() const {
        if (last_failed_exchange_status_ == nullptr) {
            throw std::runtime_error("test plugin exchange-status export is missing");
        }
        return last_failed_exchange_status_();
    }

private:
    using GetState = void (*)(std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*,
                              std::uint64_t*);

    SharedLibrary library_;
    void (*reset_)() = nullptr;
    GetState get_ = nullptr;
    std::uint64_t (*thread_violations_)() = nullptr;
    bool (*validation_started_)() = nullptr;
    void (*release_validation_)() = nullptr;
    yaddnsc_status (*last_failed_exchange_status_)() = nullptr;
};

/// In-process HTTP/1.1 server: one handler thread per connection. In hold mode
/// each request holds the connection for `hold` (or until shutdown) before
/// answering, which lets tests create slow and never-answering exchanges. In
/// Barrier mode requests are answered by a rendezvous instead of a clock.
class LoopbackHttpServer {
public:
    /// Rendezvous answer mode: each request waits until `count` requests have
    /// arrived, then all are answered together. The rendezvous is only
    /// reachable while every client holds an exchange in flight, so meeting it
    /// proves concurrency without any wall-clock budget. `give_up` bounds the
    /// wait so a client that arrives alone (the serialized regression) is
    /// answered after the deadline instead of hanging the test; barrier_met()
    /// then reports false.
    struct Barrier {
        std::uint32_t count;
        std::chrono::milliseconds give_up;
    };

    explicit LoopbackHttpServer(std::chrono::milliseconds hold) : LoopbackHttpServer(hold, std::nullopt) {}

    explicit LoopbackHttpServer(Barrier barrier) : LoopbackHttpServer(0ms, barrier) {}

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
                [[maybe_unused]] const int woke =
                    ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                ::close(wake);
            }
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        // accept_loop() reads listener_.get() in accept(); close the listener
        // only after the join so the write is ordered after it. Handler
        // threads never touch listener_.
        listener_.reset();
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

    /// Whether the rendezvous was reached rather than the give-up deadline.
    /// Meaningful only for a Barrier server, after the clients have finished.
    [[nodiscard]] bool barrier_met() const noexcept { return !barrier_missed_.load(); }

private:
    LoopbackHttpServer(std::chrono::milliseconds hold, std::optional<Barrier> barrier)
        : hold_(hold), barrier_(barrier) {
        listener_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        const int enabled = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("cannot bind loopback HTTP test listener");
        }

        socklen_t length = sizeof(address);
        ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::listen(listener_.get(), 16);
        accept_thread_ = std::jthread([this] { accept_loop(); });
    }

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
        requests_.fetch_add(1);
        if (barrier_.has_value()) {
            // Wake handlers already parked at the rendezvous: this arrival may
            // be the one that completes it.
            cv_.notify_all();
        }

        {
            std::unique_lock lock(mutex_);
            if (barrier_.has_value()) {
                const bool met = cv_.wait_for(lock, barrier_->give_up,
                                              [this] { return stop_ || requests_.load() >= barrier_->count; });
                if (!met && !stop_) {
                    barrier_missed_ = true;
                }
            } else {
                cv_.wait_for(lock, hold_, [this] { return stop_; });
            }
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
    std::optional<Barrier> barrier_;
    Utils::UniqueFd listener_;
    std::uint16_t port_{0};
    std::atomic<std::uint32_t> requests_{0};
    std::atomic<bool> barrier_missed_{false};
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

TEST(Bridge, SubmissionAfterGroupClosure_RejectsWithoutTouchingDeadGroup) {
    coro::Loop loop;
    std::shared_ptr<plugin::Bridge> bridge;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                bridge = std::make_shared<plugin::Bridge>(loop, group, http::Options{}, 100ms);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        auto call = std::make_shared<plugin::BridgeCall>();
        call->url = "http://127.0.0.1:1/";
        auto result = co_await coro::offload_joined([bridge, call] { return bridge->exchange(call); });
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, YADDNSC_STATUS_CANCELLED);
    };
    coro::run(loop, root());
}

namespace {

class ClockedBridgeStream : public net::Stream {
public:
    explicit ClockedBridgeStream(coro::Duration delay) : delay_(delay) {}

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        co_await coro::sleep_for(delay_);
        co_return {};
    }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> buffer) override {
        constexpr std::string_view response = "HTTP/1.1 204 No Content\r\n\r\n";
        const auto count = std::min(buffer.size(), response.size() - cursor_);
        std::memcpy(buffer.data(), response.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t>) override { co_return {}; }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

private:
    coro::Duration delay_;
    std::size_t cursor_ = 0;
};

class ClockedBridgeFactory : public net::StreamFactory {
public:
    explicit ClockedBridgeFactory(coro::Duration delay) : delay_(delay) {}

    std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t, const net::ConnectOptions&,
                                            const net::TlsOptions&, std::shared_ptr<const net::TlsContext>) override {
        ++calls_;
        return std::make_unique<ClockedBridgeStream>(delay_);
    }

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        ++calls_;
        return std::make_unique<ClockedBridgeStream>(delay_);
    }

    int calls_ = 0;

private:
    coro::Duration delay_;
};

/// A defect during shielded transport cleanup remains a host defect after cancel.
class DefectiveCleanupStream final : public ClockedBridgeStream {
public:
    explicit DefectiveCleanupStream(bool& entered) : ClockedBridgeStream(0ms), entered_(entered) {}

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        entered_ = true;
        co_await coro::non_cancellable([](coro::CancelScope&) -> coro::Task<void> {
            co_await coro::sleep_for(10ms);
            throw std::runtime_error("host transport cleanup defect");
        });
        co_return {};
    }

private:
    bool& entered_;
};

class DefectiveCleanupFactory final : public ClockedBridgeFactory {
public:
    DefectiveCleanupFactory() : ClockedBridgeFactory(0ms) {}

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        return std::make_unique<DefectiveCleanupStream>(entered);
    }

    bool entered = false;
};

class RetiringStream final : public ClockedBridgeStream {
public:
    RetiringStream(bool first, bool& release, int& alive)
        : ClockedBridgeStream(0ms), first_(first), release_(release), alive_(alive) {
        ++alive_;
    }

    ~RetiringStream() override { --alive_; }

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        if (first_) {
            co_await coro::non_cancellable([&](coro::CancelScope&) -> coro::Task<void> {
                while (!release_) {
                    co_await coro::checkpoint();
                }
            });
            co_await coro::checkpoint();
        } else {
            co_await coro::sleep_for(30s);
        }
        co_return {};
    }

private:
    bool first_;
    bool& release_;
    int& alive_;
};

class RetiringFactory final : public ClockedBridgeFactory {
public:
    RetiringFactory() : ClockedBridgeFactory(0ms) {}

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        return std::make_unique<RetiringStream>(++calls_ == 1, release, alive);
    }

    bool release = false;
    int alive = 0;
};

struct SlowResumeSignals {
    explicit SlowResumeSignals(coro::Loop& owner) : loop(owner) {}

    coro::Loop& loop;
    coro::Notification second_started;
    coro::Notification parked;
    coro::Notification second_retired;
    coro::Notification resume_entered;
    bool second_cancelled = false;
    unsigned active_streams = 0;
};

class SlowResumeStream final : public ClockedBridgeStream {
public:
    SlowResumeStream(SlowResumeSignals& signals, bool second)
        : ClockedBridgeStream(0ms), signals_(signals), second_(second) {
        ++signals_.active_streams;
    }

    ~SlowResumeStream() override {
        --signals_.active_streams;
        if (second_) {
            signals_.second_retired.notify();
        }
    }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> buffer) override {
        if (second_) {
            signals_.second_started.notify();
            try {
                co_await signals_.parked.wait();
            } catch (const coro::Cancelled&) {
                signals_.second_cancelled = true;
                throw;
            }
            co_return std::unexpected(net::IoError::CONNECTION_FAILED);
        }
        // The first completion cannot enter resume until the second exchange
        // is actually waiting in transport I/O.
        co_await signals_.second_started.wait();
        co_return co_await ClockedBridgeStream::read_some(buffer);
    }

private:
    SlowResumeSignals& signals_;
    bool second_;
};

class SlowResumeFactory final : public ClockedBridgeFactory {
public:
    explicit SlowResumeFactory(SlowResumeSignals& signals) : ClockedBridgeFactory(0ms), signals_(signals) {}

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t port,
                                            const net::ConnectOptions&) override {
        return std::make_unique<SlowResumeStream>(signals_, port == 2);
    }

private:
    SlowResumeSignals& signals_;
};

// Keep loop deadlines inert while workers rendezvous through the loop inbox.
// Unlike ManualClock, this clock must not jump to a deadline while a real
// offload is pending. The test runner's timeout is the hang watchdog.
class PausedClock final : public coro::Clock {
public:
    [[nodiscard]] coro::TimePoint now() const noexcept override { return {}; }
};

struct HeldResponse {
    coro::Notification entered;
    coro::Notification release;
    unsigned reads = 0;
    unsigned cancelled = 0;
    unsigned responses = 0;
    unsigned alive = 0;
};

class HeldResponseStream final : public ClockedBridgeStream {
public:
    explicit HeldResponseStream(HeldResponse& state) : ClockedBridgeStream(0ms), state_(state) { ++state_.alive; }

    ~HeldResponseStream() override { --state_.alive; }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> buffer) override {
        ++state_.reads;
        state_.entered.notify();
        try {
            co_await state_.release.wait();
        } catch (const coro::Cancelled&) {
            ++state_.cancelled;
            throw;
        }
        ++state_.responses;
        co_return co_await ClockedBridgeStream::read_some(buffer);
    }

private:
    HeldResponse& state_;
};

class HeldResponseFactory final : public ClockedBridgeFactory {
public:
    explicit HeldResponseFactory(HeldResponse& state) : ClockedBridgeFactory(0ms), state_(state) {}

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t port,
                                            const net::ConnectOptions&) override {
        ++calls_;
        if (port == 1) {
            return std::make_unique<HeldResponseStream>(state_);
        }
        return std::make_unique<ClockedBridgeStream>(0ms);
    }

private:
    HeldResponse& state_;
};

}  // namespace

TEST(Bridge, ManualClock_RemainingBudgetUsesLoopEpoch) {
    auto factory = std::make_shared<ClockedBridgeFactory>(1h);
    http::Options options;
    options.factory = factory;
    options.connect_timeout = 0ms;
    auto state = std::make_shared<const plugin::detail::BridgeState>(plugin::detail::BridgeState{options, 5s});
    auto call = std::make_shared<plugin::BridgeCall>();
    call->url = "http://127.0.0.1/";
    // Dispatch has already consumed all but 1s of the original 5s budget.
    // The real-time margin tolerates a descheduled runner; fake I/O is virtual.
    call->worker_deadline = std::chrono::steady_clock::now() + 1s;
    auto result = call->promise.get_future();
    const coro::TimePoint start{10s};
    coro::ManualClock clock{start};
    coro::Loop loop{clock};
    coro::run(loop, plugin::detail::serve_exchange(state, call));
    const auto response = result.get();
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_GT(clock.now(), start);
    EXPECT_LE(clock.now() - start, 1s);
    EXPECT_EQ(factory->calls_, 1);
    EXPECT_EQ(call->live_scope, nullptr);
}

TEST(Bridge, ExpiredWorkerBudget_DoesNotStartHttp) {
    auto factory = std::make_shared<ClockedBridgeFactory>(1ms);
    http::Options options;
    options.factory = factory;
    auto state = std::make_shared<const plugin::detail::BridgeState>(plugin::detail::BridgeState{options, 5s});
    auto call = std::make_shared<plugin::BridgeCall>();
    call->url = "http://127.0.0.1/";
    call->worker_deadline = std::chrono::steady_clock::now() - 1ms;
    auto result = call->promise.get_future();
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::run(loop, plugin::detail::serve_exchange(state, call));
    const auto response = result.get();
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(factory->calls_, 0);
    EXPECT_EQ(clock.now(), coro::TimePoint{});
}

TEST(Bridge, ManualClock_ResponseBeforeDeadlineCompletes) {
    auto factory = std::make_shared<ClockedBridgeFactory>(1ms);
    http::Options options;
    options.factory = factory;
    auto state = std::make_shared<const plugin::detail::BridgeState>(plugin::detail::BridgeState{options, 5s});
    auto call = std::make_shared<plugin::BridgeCall>();
    call->url = "http://127.0.0.1/";
    call->worker_deadline = std::chrono::steady_clock::now() + 5s;
    auto result = call->promise.get_future();
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::run(loop, plugin::detail::serve_exchange(state, call));
    const auto response = result.get();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 204);
    EXPECT_EQ(clock.now(), coro::TimePoint{1ms});
    EXPECT_EQ(call->live_scope, nullptr);
}

TEST(Bridge, CancelBeforeScopePublication_DoesNotStartHttp) {
    auto factory = std::make_shared<ClockedBridgeFactory>(1ms);
    http::Options options;
    options.factory = factory;
    auto state = std::make_shared<const plugin::detail::BridgeState>(plugin::detail::BridgeState{options, 5s});
    auto call = std::make_shared<plugin::BridgeCall>();
    call->url = "http://127.0.0.1/";
    call->worker_deadline = std::chrono::steady_clock::now() + 5s;
    auto result = call->promise.get_future();
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                auto serving = group.spawn(plugin::detail::serve_exchange(state, call));
                co_await coro::checkpoint();
                // serve_exchange has passed its entry check, but with_cancel_scope
                // has not published live_scope yet. Mirror cancel's latched flag.
                EXPECT_EQ(call->live_scope, nullptr);
                call->cancelled.store(true, std::memory_order_release);
                co_await serving;
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, task());
    const auto response = result.get();
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(factory->calls_, 0);
    EXPECT_EQ(call->live_scope, nullptr);
}

TEST(Bridge, WorkerDeadline_CancelsAndDrainsWithoutAResponse) {
    PausedClock clock;
    coro::Loop loop{clock};
    HeldResponse held;
    auto factory = std::make_shared<HeldResponseFactory>(held);
    http::Options options;
    options.factory = factory;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                auto bridge = std::make_shared<plugin::Bridge>(loop, group, options, 30ms);
                auto call = std::make_shared<plugin::BridgeCall>();
                call->url = "http://127.0.0.1:1/";
                auto result = co_await coro::offload_joined([bridge, call] { return bridge->exchange(call); });
                EXPECT_FALSE(result.has_value());
                EXPECT_EQ(result.error().code, YADDNSC_STATUS_NETWORK_ERROR);
                EXPECT_TRUE(call->cancelled.load(std::memory_order_acquire));
            },
            coro::NurseryPolicy::SUPERVISOR);
        // Only the worker's real deadline can expire: loop time is paused and
        // no response is released. A descheduled loop may reject at admission
        // rather than enter I/O; this does not claim running-I/O coverage.
        EXPECT_EQ(held.responses, 0u);
        EXPECT_EQ(held.cancelled, held.reads);
        EXPECT_EQ(held.alive, 0u);
    };
    coro::run(loop, root());
}

TEST_F(CoroDriverGatewayTest, AsyncDriverCompletesThroughTheGateway) {
    LoopbackHttpServer server{0ms};
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                const std::string url = fmt::format("http://127.0.0.1:{}", server.port());
                result = co_await gateway.update("async_fixture", make_command(url));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(server.request_count(), 1u);
}

TEST_F(CoroDriverGatewayTest, AsyncSdkDriverCancellationDrainsTheRequest) {
    PausedClock clock;
    HeldResponse held;
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_SDK_PATH)));
    coro::Loop loop{clock};
    auto options = default_options(30s);
    options.http.factory = std::make_shared<HeldResponseFactory>(held);
    bool saw_request = false;
    const std::string url = "http://127.0.0.1:1/";

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                co_await coro::with_nursery(
                    [&](coro::Nursery& cycle) -> coro::Task<void> {
                        cycle.spawn(gateway.update("async_sdk_driver", make_command(url)));
                        co_await held.entered.wait();
                        saw_request = true;
                        cycle.cancel();
                        co_return;
                    },
                    coro::NurseryPolicy::SUPERVISOR);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
    EXPECT_TRUE(saw_request);
    EXPECT_EQ(held.reads, 1u);
}

TEST_F(CoroDriverGatewayTest, AsyncWaitDoesNotBlockTheOnlyWorkerAndSyncModeCoexists) {
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    PausedClock clock;
    coro::Loop loop{clock};
    loop.set_offload_workers(1);
    HeldResponse held;
    auto factory = std::make_shared<HeldResponseFactory>(held);
    auto options = default_options();
    options.http.factory = factory;
    std::expected<void, domain::DriverError> sync_result;
    bool async_returned = false;
    auto async_update = [&](plugin::DriverGateway& gateway) -> coro::Task<void> {
        [[maybe_unused]] auto result = co_await gateway.update("async_fixture", make_command("http://127.0.0.1:1/"));
        async_returned = true;
    };

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                co_await coro::with_nursery(
                    [&](coro::Nursery& cycle) -> coro::Task<void> {
                        cycle.spawn_discard(async_update(gateway));
                        co_await held.entered.wait();
                        sync_result = co_await gateway.update(DRIVER_NAME, make_command(exchange_params(2)));
                        // The only worker completed sync work while async I/O
                        // was still parked, not after an async deadline expired.
                        EXPECT_FALSE(async_returned);
                        EXPECT_EQ(held.alive, 1u);
                        EXPECT_EQ(held.cancelled, 0u);
                        EXPECT_EQ(held.responses, 0u);
                        cycle.cancel();
                    },
                    coro::NurseryPolicy::SUPERVISOR);
            },
            coro::NurseryPolicy::SUPERVISOR);
    });

    ASSERT_TRUE(sync_result.has_value()) << sync_result.error().message;
    EXPECT_FALSE(async_returned);
    EXPECT_EQ(factory->calls_, 2);
    EXPECT_EQ(held.reads, 1u);
    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
}

TEST_F(CoroDriverGatewayTest, CancellationStopsHttpBeforeSlowResumeReturnsAndDestroyWaitsForJoin) {
    auto library = SharedLibrary::open(std::string(ASYNC_SLOW_RESUME_PATH));
    ASSERT_TRUE(library.has_value()) << library.error();
    const auto configure =
        reinterpret_cast<void (*)(void*, void (*)(void*))>(library->resolve("async_slow_resume_configure"));  // NOLINT
    const auto release = reinterpret_cast<void (*)()>(library->resolve("async_slow_resume_release"));         // NOLINT
    const auto get_state =
        reinterpret_cast<void (*)(bool*, bool*, unsigned*)>(library->resolve("async_slow_resume_state"));  // NOLINT
    ASSERT_NE(configure, nullptr);
    ASSERT_NE(release, nullptr);
    ASSERT_NE(get_state, nullptr);
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_SLOW_RESUME_PATH)));

    PausedClock clock;
    coro::Loop loop{clock};
    loop.set_offload_workers(1);
    SlowResumeSignals signals{loop};
    configure(&signals, [](void* context) {
        auto& state = *static_cast<SlowResumeSignals*>(context);
        state.loop.post([&state] { state.resume_entered.notify(); });
    });
    auto options = default_options(30s);
    options.http.factory = std::make_shared<SlowResumeFactory>(signals);
    bool checked_order = false;
    coro::CancelScope* caller_scope = nullptr;

    auto control = [&]() -> coro::Task<void> {
        struct ReleaseWorker {
            void (*release)();
            ~ReleaseWorker() { release(); }
        } release_worker{release};
        co_await signals.resume_entered.wait();
        EXPECT_NE(caller_scope, nullptr);
        if (caller_scope != nullptr) {
            caller_scope->cancel();
        }
        co_await signals.second_retired.wait();
        bool did_return = false;
        bool saw_cancel = false;
        unsigned destroyed = 0;
        get_state(&did_return, &saw_cancel, &destroyed);
        EXPECT_TRUE(signals.second_cancelled);
        EXPECT_FALSE(did_return);
        EXPECT_EQ(destroyed, 0u);
        checked_order = signals.second_cancelled && !did_return && destroyed == 0;
        // release_worker lets the joined step return only after
        // the assertions above, including on a failed test path.
    };

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& caller) -> coro::Task<void> {
                    caller_scope = &caller;
                    struct ScopeBorrow {
                        coro::CancelScope*& slot;
                        ~ScopeBorrow() { slot = nullptr; }
                    } scope_borrow{caller_scope};
                    group.spawn_discard(control());
                    [[maybe_unused]] auto result = co_await gateway.update("async_slow_resume", make_command("{}"));
                    ADD_FAILURE() << "cancelled subscriber must not consume an update result";
                });
                EXPECT_TRUE(outcome.cancelled);
            },
            coro::NurseryPolicy::SUPERVISOR);
    });

    bool did_return = false;
    bool saw_cancel = false;
    unsigned destroyed = 0;
    get_state(&did_return, &saw_cancel, &destroyed);
    EXPECT_TRUE(checked_order);
    EXPECT_TRUE(did_return);
    EXPECT_TRUE(saw_cancel);
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(signals.active_streams, 0u);
}

TEST_F(CoroDriverGatewayTest, AsyncStartCancelInOneStepDoesNotSend) {
    LoopbackHttpServer server{0ms};
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;
    const std::string params = fmt::format("http://127.0.0.1:{}|cancel", server.port());

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                result = co_await gateway.update("async_fixture", make_command(params));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(server.request_count(), 0u);
}

TEST_F(CoroDriverGatewayTest, AsyncSdkDriverCompletesThroughTheGateway) {
    LoopbackHttpServer server{0ms};
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_SDK_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> update_result;
    std::expected<void, domain::DriverError> valid;
    std::expected<void, domain::DriverError> invalid;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                const std::string url = fmt::format("http://127.0.0.1:{}", server.port());
                update_result = co_await gateway.update("async_sdk_driver", make_command(url));
                valid = co_await gateway.validate_config("async_sdk_driver", "{}");
                invalid = co_await gateway.validate_config("async_sdk_driver", "");
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_TRUE(update_result.has_value()) << update_result.error().message;
    EXPECT_EQ(server.request_count(), 1u);
    ASSERT_TRUE(valid.has_value()) << valid.error().message;
    ASSERT_FALSE(invalid.has_value());
}

TEST_F(CoroDriverGatewayTest, AsyncDriverPropagatesHttpFailure) {
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                result = co_await gateway.update("async_fixture", make_command("http://127.0.0.1:1/"));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
}

TEST_F(CoroDriverGatewayTest, AsyncDriverValidatesThroughTheGateway) {
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> valid;
    std::expected<void, domain::DriverError> invalid;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                valid = co_await gateway.validate_config("async_fixture", "{}");
                invalid = co_await gateway.validate_config("async_fixture", "");
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_TRUE(valid.has_value()) << (valid.has_value() ? std::string{} : valid.error().message);
    ASSERT_FALSE(invalid.has_value());
}

TEST_F(CoroDriverGatewayTest, AsyncValidatePublishesCreateFailureWithoutHanging) {
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_CREATE_FAILS_PATH)));
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;
    bool completed = false;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                result = co_await gateway.validate_config("async_create_fails", "{}");
                completed = true;
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    EXPECT_TRUE(completed);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("create rejected"), std::string::npos) << result.error().message;
}

TEST_F(CoroDriverGatewayTest, AsyncDriverCancellationDrainsTheRequest) {
    PausedClock clock;
    HeldResponse held;
    ASSERT_NO_THROW(catalog_.load_driver(std::string(ASYNC_DRIVER_PATH)));
    coro::Loop loop{clock};
    auto options = default_options(30s);
    options.http.factory = std::make_shared<HeldResponseFactory>(held);
    bool saw_request = false;
    const std::string url = "http://127.0.0.1:1/";

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                co_await coro::with_nursery(
                    [&](coro::Nursery& cycle) -> coro::Task<void> {
                        cycle.spawn(gateway.update("async_fixture", make_command(url)));
                        co_await held.entered.wait();
                        saw_request = true;
                        cycle.cancel();
                        co_return;
                    },
                    coro::NurseryPolicy::SUPERVISOR);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
    EXPECT_TRUE(saw_request);
    EXPECT_EQ(held.reads, 1u);
}

TEST_F(CoroDriverGatewayTest, HttpExchangeRoundTripsThroughTheBridge) {
    LoopbackHttpServer server{0ms};
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;

    const std::string params = exchange_params(server.port(), 2);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                result = co_await gateway.update(DRIVER_NAME, make_command(params));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(server.request_count(), 2u);
}

TEST_F(CoroDriverGatewayTest, ExchangeFailureObservationRecordsWorkerDeadlineAndResets) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    ASSERT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_OK);

    PausedClock clock;
    coro::Loop loop{clock};
    HeldResponse held;
    auto options = default_options(0ms);
    options.http.factory = std::make_shared<HeldResponseFactory>(held);
    std::expected<void, domain::DriverError> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                // An already-expired worker budget exercises the fallback
                // status deterministically, without waiting on a real timer.
                result = co_await gateway.update(DRIVER_NAME, make_command(exchange_params(1)));
            },
            coro::NurseryPolicy::SUPERVISOR);
    });

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(held.alive, 0u);
    control.reset();
    EXPECT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_OK);
}

// ---------------------------------------------------------------------------
// Concurrent cycles
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, ConcurrentCyclesOfOneDriverRunInParallel) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    // Each request blocks until both have arrived: the rendezvous is only
    // reachable while both cycles hold an exchange in flight, so meeting it is
    // the concurrency proof — no wall-clock budget involved. A serialized pair
    // never meets it and is answered after the give-up instead of hanging. The
    // bridge budget (30s) far exceeds the give-up so that in the serialized
    // case the barrier — not the budget — is what releases and fails the test.
    LoopbackHttpServer server{LoopbackHttpServer::Barrier{.count = 2, .give_up = 10s}};

    coro::Loop loop;
    std::expected<void, domain::DriverError> first_result;
    std::expected<void, domain::DriverError> second_result;
    const std::string params = exchange_params(server.port(), 1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group,
                                                                                  default_options(30s)));
                {
                    auto first = group.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                    second_result = co_await gateway.update(DRIVER_NAME, make_command(params));
                    first_result = co_await first;
                };
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    ASSERT_TRUE(first_result.has_value()) << first_result.error().message;
    ASSERT_TRUE(second_result.has_value()) << second_result.error().message;
    // Both exchanges were in flight at once — the ABI allows concurrent cycles
    // on distinct instances, and every cycle creates a fresh one. A serialized
    // pair would have missed the rendezvous and hit the give-up deadline.
    EXPECT_TRUE(server.barrier_met());
    EXPECT_EQ(server.request_count(), 2u);
    // Both cycles ran the full create → update → destroy sequence; the worker
    // destroys the instance before its result reaches the loop, so the counts
    // are settled by the time run_loop returns.
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 2u);
    EXPECT_EQ(state.updates, 2u);
    EXPECT_EQ(state.destroys, 2u);
}

// ---------------------------------------------------------------------------
// Abandon
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, AbandonedCycleCancelsItsInFlightExchange) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    ASSERT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_OK);

    PausedClock clock;
    coro::Loop loop{clock};
    HeldResponse held;
    auto options = default_options(30s);
    options.http.factory = std::make_shared<HeldResponseFactory>(held);
    bool saw_request = false;
    const std::string params = exchange_params(1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                // A nested group scopes the abandon to this one update; the outer
                // scope, and with it the gateway's bridge, stay live.
                co_await coro::with_nursery(
                    [&](coro::Nursery& cycle) -> coro::Task<void> {
                        // No result is needed: Handle join would rethrow Cancelled,
                        // while group exit drains and absorbs its own cancellation.
                        cycle.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                        co_await held.entered.wait();
                        saw_request = true;
                        cycle.cancel();
                        co_return;
                    },
                    coro::NurseryPolicy::SUPERVISOR);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    // Stream cancellation alone is insufficient: a worker deadline also
    // cancels I/O, but returns NETWORK_ERROR rather than CANCELLED to the plugin.
    EXPECT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
    EXPECT_TRUE(saw_request);
    // run() has drained every submitted worker, so ABI destroy is already
    // complete. Polling here would hide a broken worker-lifetime contract.
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 1u);
    EXPECT_EQ(state.updates, 1u);
    EXPECT_EQ(state.destroys, 1u);
    EXPECT_EQ(held.reads, 1u);
}

TEST_F(CoroDriverGatewayTest, AbandonedCycleWindsDownAndFreesTheNextUpdate) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    ASSERT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_OK);

    PausedClock clock;
    coro::Loop loop{clock};
    HeldResponse held;
    auto factory = std::make_shared<HeldResponseFactory>(held);
    auto options = default_options(30s);
    options.http.factory = factory;
    std::expected<void, domain::DriverError> second_result;
    bool saw_request = false;
    const std::string params = exchange_params(1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                // A nested group scopes the abandon to this one update; the outer
                // scope, and with it the gateway's bridge, stay live. Spawning the
                // member coroutine directly keeps its frame off GCC's elision
                // path, which would hand spawn() a stack frame to own.
                co_await coro::with_nursery(
                    [&](coro::Nursery& cycle) -> coro::Task<void> {
                        cycle.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                        co_await held.entered.wait();
                        saw_request = true;
                        cycle.cancel();
                        co_return;
                    },
                    coro::NurseryPolicy::SUPERVISOR);
                // The abandoned cycle's in-flight exchange is cancelled and its
                // wind-down runs on the worker; this update does not wait for it.
                second_result = co_await gateway.update(DRIVER_NAME, make_command(exchange_params(2)));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    // The first cycle's exchange was in flight when the abandon landed, so the
    // cancel reached a running cycle rather than dropping a queued one.
    EXPECT_TRUE(saw_request);
    EXPECT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_CANCELLED);
    ASSERT_TRUE(second_result.has_value()) << second_result.error().message;

    // Both cycles ran the full create → update → destroy sequence: the
    // abandoned one by returning CANCELLED from its aborted exchange, the
    // second one with a released answer.
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 2u);
    EXPECT_EQ(state.updates, 2u);
    EXPECT_EQ(state.destroys, 2u);
    // The abandoned cycle reached transport I/O before cancellation, and
    // the second cycle ran its own exchange.
    EXPECT_EQ(factory->calls_, 2);
    EXPECT_EQ(held.reads, 1u);
    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
}

TEST_F(CoroDriverGatewayTest, AbandonedQueuedCycle_DoesNotEnterTheDriver) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();

    coro::Loop loop;
    loop.set_offload_workers(1);
    HeldResponse held;
    auto factory = std::make_shared<HeldResponseFactory>(held);
    auto options = default_options(30s);
    options.http.factory = factory;
    std::expected<void, domain::DriverError> first_result;
    std::expected<void, domain::DriverError> third_result;
    bool abandoned_returned = false;
    bool cancelled = false;

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                auto first = group.spawn(gateway.update(DRIVER_NAME, make_command(exchange_params(1))));
                co_await held.entered.wait();
                // The first worker stays behind the response gate until this
                // queued subscriber's cancellation has actually unwound.
                const auto outcome = co_await coro::with_timeout(20ms, [&]() -> coro::Task<void> {
                    [[maybe_unused]] auto result =
                        co_await gateway.update(DRIVER_NAME, make_command(exchange_params(2)));
                    abandoned_returned = true;
                });
                cancelled = outcome.timed_out;
                EXPECT_TRUE(cancelled);
                EXPECT_FALSE(abandoned_returned);
                EXPECT_EQ(control.state().creates, 1u);
                EXPECT_EQ(held.responses, 0u);
                EXPECT_EQ(held.cancelled, 0u);
                held.release.notify();
                first_result = co_await first;
                // Drain the queue behind the cancelled job before counting ABI
                // entries. A lost queue cancellation would create a third one.
                third_result = co_await gateway.update(DRIVER_NAME, make_command(exchange_params(2)));
            },
            coro::NurseryPolicy::SUPERVISOR);
    });

    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(abandoned_returned);
    ASSERT_TRUE(first_result.has_value()) << first_result.error().message;
    ASSERT_TRUE(third_result.has_value()) << third_result.error().message;
    const PluginState state = control.state();
    EXPECT_EQ(state.creates, 2u);
    EXPECT_EQ(state.updates, 2u);
    EXPECT_EQ(state.destroys, 2u);
    EXPECT_EQ(factory->calls_, 2);
    EXPECT_EQ(held.responses, 1u);
    EXPECT_EQ(held.alive, 0u);
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
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog, logger, loop, group, default_options()));
                result = co_await gateway.update("throwing", make_command(R"({})"));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
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
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    ASSERT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_OK);

    PausedClock clock;
    coro::Loop loop{clock};
    HeldResponse held;
    auto options = default_options(30s);
    options.http.factory = std::make_shared<HeldResponseFactory>(held);
    bool saw_request = false;
    const std::string params = exchange_params(1);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                {
                    group.spawn(gateway.update(DRIVER_NAME, make_command(params)));
                    // Cancel bridge I/O, then drain before gateway is destroyed.
                    co_await held.entered.wait();
                    saw_request = true;
                    group.cancel();
                };
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    // Check the worker's actual exchange result after drain: deadline expiry
    // can also cancel the stream, but must not count as shutdown cancellation.
    EXPECT_EQ(control.last_failed_exchange_status(), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(held.cancelled, 1u);
    EXPECT_EQ(held.responses, 0u);
    EXPECT_EQ(held.alive, 0u);
    EXPECT_TRUE(saw_request);
}

TEST_F(CoroDriverGatewayTest, PluginCancelledStatus_BecomesAnErrorValueOnALiveScope) {
    coro::Loop loop;
    std::expected<void, domain::DriverError> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                result = co_await gateway.update(DRIVER_NAME, make_command(R"({"op":"fail","status":"cancelled"})"));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    });
    // A driver-reported cancellation on a live scope is one failed cycle, not
    // the end of the subdomain task: the caller gets an error value and the
    // schedule retries on the next interval (the legacy mapping). Only a
    // host-originated cancel still propagates as coro::Cancelled — that path
    // is covered by the AbandonedCycle tests above.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
}

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

TEST_F(CoroDriverGatewayTest, ValidationCancelledStatus_IsFailureValueWithoutCancellingHost) {
    coro::Loop loop;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(
                    owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, default_options()));
                {
                    auto result = co_await gateway.validate_config(DRIVER_NAME, R"({"op":"cancel_validate"})");
                    EXPECT_FALSE(result.has_value());
                    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
                    EXPECT_FALSE(group.scope().cancelled());
                    // Foreign failure must not abort validation of the next entry.
                    EXPECT_TRUE((co_await gateway.validate_config(DRIVER_NAME, R"({})")).has_value());
                };
            },
            coro::NurseryPolicy::SUPERVISOR);
    });
}

TEST(CoroDriverGatewayValidate, MissingValidateEntryFailsConfigTest) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(NO_VALIDATE_FIXTURE)));
    NullLogger logger;

    coro::Loop loop;
    std::expected<void, domain::DriverError> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog, logger, loop, group, default_options()));
                result = co_await gateway.validate_config("no_validate", R"({"anything":true})");
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    });

    // The plugin predates the optional entry. config test fails because the
    // host cannot confirm driver_params (the ABI contract), not because the
    // plugin rejected anything.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_THAT(result.error().message, ::testing::HasSubstr("does not provide yaddnsc_driver_validate"));
}

TEST_F(CoroDriverGatewayTest, HostDefect_WithActiveCaller_IsRethrownAfterCycleDrain) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    auto factory = std::make_shared<DefectiveCleanupFactory>();
    auto options = default_options();
    options.http.factory = factory;
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                {
                    EXPECT_THROW(co_await gateway.update(DRIVER_NAME, make_command(exchange_params(1, 1))),
                                 std::runtime_error);
                };
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
    EXPECT_TRUE(factory->entered);
    EXPECT_EQ(control.state().destroys, 1u);
}

TEST_F(CoroDriverGatewayTest, HostDefect_AfterCallerCancellation_ReachesRootErrorBoundary) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    auto factory = std::make_shared<DefectiveCleanupFactory>();
    auto options = default_options();
    options.http.factory = factory;
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                {
                    co_await coro::with_nursery(
                        [&](coro::Nursery& callers) -> coro::Task<void> {
                            callers.spawn(gateway.update(DRIVER_NAME, make_command(exchange_params(1, 1))));
                            while (!factory->entered) {
                                co_await coro::checkpoint();
                            }
                            callers.cancel();
                        },
                        coro::NurseryPolicy::SUPERVISOR);
                };
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    EXPECT_THROW(coro::run(loop, root()), coro::GroupError);
    EXPECT_EQ(control.state().destroys, 1u);
}

TEST(Bridge, UnboundCycle_RejectsWithoutStartingHttp) {
    coro::Loop loop;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            auto bridge = std::make_shared<plugin::Bridge>(loop, group, http::Options{}, 100ms);
            auto call = std::make_shared<plugin::BridgeCall>();
            call->cycle = std::make_shared<plugin::CallState>();
            call->url = "http://127.0.0.1:1/never-admit";
            auto response = co_await coro::offload_joined([bridge, call] { return bridge->exchange(call); });
            EXPECT_FALSE(response.has_value());
            status = response ? YADDNSC_STATUS_OK : response.error().code;
        });
    };
    coro::run(loop, root());
    EXPECT_EQ(status, YADDNSC_STATUS_CANCELLED);
}

TEST_F(CoroDriverGatewayTest, UpdateAndValidation_CreateEntryAndDestroyRemainOnOneWorkerThread) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group));
                {
                    EXPECT_TRUE(co_await gateway.update(DRIVER_NAME, make_command(R"({"op":"success"})")));
                    EXPECT_TRUE(co_await gateway.validate_config(DRIVER_NAME, R"({"op":"success"})"));
                };
            });
    };
    coro::run(loop, root());
    EXPECT_EQ(control.state().creates, 2U);
    EXPECT_EQ(control.state().destroys, 2U);
    EXPECT_EQ(control.thread_violations(), 0U);
}

TEST_F(CoroDriverGatewayTest, Validation_HostCancellationReturnsBeforeUncooperativeWorkerThenDrains) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway = *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group));
                {
                    const auto outcome =
                        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                            auto cancel = [&]() -> coro::Task<void> {
                                while (!control.validation_started()) {
                                    co_await coro::checkpoint();
                                }
                                scope.cancel();
                            };
                            group.spawn_discard(cancel());
                            [[maybe_unused]] auto result =
                                co_await gateway.validate_config(DRIVER_NAME, R"({"op":"wait_validate"})");
                            ADD_FAILURE() << "validation must observe host cancellation";
                        });
                    EXPECT_TRUE(outcome.cancelled);
                    EXPECT_EQ(control.state().destroys, 0U);
                    control.release_validation();
                };
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
    EXPECT_EQ(control.state().destroys, 1U);
    EXPECT_EQ(control.thread_violations(), 0U);
}

TEST_F(CoroDriverGatewayTest, CompletionPublishedBeforeCallerResumes_CancellationWinsAndPreservesDefectBoundary) {
    for (bool defect : {false, true}) {
        PluginControl control{std::string(PLUGIN_PATH)};
        control.reset();
        auto options = default_options();
        if (defect) {
            options.http.factory = std::make_shared<DefectiveCleanupFactory>();
        }
        coro::Loop loop;
        bool cancelled_queued_result = false;
        auto root = [&]() -> coro::Task<void> {
            co_await coro::with_nursery(
                std::unique_ptr<plugin::DriverGateway>{},
                [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                    auto& gateway =
                        *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                    {
                        const auto outcome =
                            co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                                auto task = gateway.update(
                                    DRIVER_NAME, make_command(defect ? exchange_params(1) : R"({"op":"success"})"));
                                // Test-only frame observation: transfer ownership back to a
                                // Task immediately; no production hook or detached frame.
                                const auto frame = coro::detail::TaskAccess::release(task);
                                task = frame.promise().get_return_object();
                                auto cancel = [&]() -> coro::Task<void> {
                                    // This observer is queued before the gateway's first
                                    // resume. Arm only after its initial result subscription.
                                    while (frame.promise().exec->resume_queued()) {
                                        co_await coro::checkpoint();
                                    }
                                    while (!frame.promise().exec->resume_queued()) {
                                        co_await coro::checkpoint();
                                    }
                                    cancelled_queued_result = true;
                                    scope.cancel();
                                };
                                group.spawn_discard(cancel());
                                [[maybe_unused]] auto result = co_await std::move(task);
                                ADD_FAILURE() << "cancelled subscriber must not consume the published result";
                            });
                        EXPECT_TRUE(outcome.cancelled);
                    };
                },
                coro::NurseryPolicy::SUPERVISOR);
        };
        if (defect) {
            EXPECT_THROW(coro::run(loop, root()), coro::GroupError);
        } else {
            EXPECT_NO_THROW(coro::run(loop, root()));
        }
        EXPECT_TRUE(cancelled_queued_result);
        EXPECT_EQ(control.state().destroys, 1U);
    }
}

TEST_F(CoroDriverGatewayTest, OneCycle_TimedOutHttpRemainsOwnedWhileSecondExchangeStartsAndCancels) {
    PluginControl control{std::string(PLUGIN_PATH)};
    control.reset();
    auto factory = std::make_shared<RetiringFactory>();
    auto options = default_options(100ms);
    options.http.factory = factory;
    options.http.connect_timeout = 0ms;
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog_, logger_, loop, group, options));
                {
                    const auto outcome =
                        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                            auto cancel = [&]() -> coro::Task<void> {
                                while (factory->calls_ < 2) {
                                    co_await coro::checkpoint();
                                }
                                EXPECT_EQ(factory->alive, 2);
                                scope.cancel();
                            };
                            group.spawn_discard(cancel());
                            [[maybe_unused]] auto result = co_await gateway.update(
                                DRIVER_NAME,
                                make_command(R"({"op":"exchange_after_timeout","url":"http://127.0.0.1/"})"));
                        });
                    EXPECT_TRUE(outcome.cancelled);
                    EXPECT_GT(factory->alive, 0);
                    factory->release = true;
                };
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
    EXPECT_EQ(factory->calls_, 2);
    EXPECT_EQ(factory->alive, 0);
    EXPECT_EQ(control.state().destroys, 1U);
    EXPECT_EQ(control.thread_violations(), 0U);
}

TEST(Bridge, WithoutCycle_HostDefectIsReportedToGroupAndWorker) {
    coro::Loop loop;
    http::Options options;
    options.factory = std::make_shared<DefectiveCleanupFactory>();
    yaddnsc_status status = YADDNSC_STATUS_OK;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                auto bridge = std::make_shared<plugin::Bridge>(loop, group, options, 5s);
                auto call = std::make_shared<plugin::BridgeCall>();
                call->url = "http://127.0.0.1/";
                const auto response = co_await coro::offload_joined([bridge, call] { return bridge->exchange(call); });
                EXPECT_FALSE(response);
                if (!response) {
                    status = response.error().code;
                }
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    EXPECT_THROW(coro::run(loop, root()), coro::GroupError);
    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
}

TEST(Bridge, CycleLiveSet_DoesNotKeepRequestOrOwnerAliveWithoutRetirementErase) {
    auto cycle = std::make_shared<plugin::CallState>();
    auto call = std::make_shared<plugin::BridgeCall>();
    call->cycle = cycle;
    cycle->calls.push_back(call);
    const std::weak_ptr<plugin::CallState> owner = cycle;
    const std::weak_ptr<plugin::BridgeCall> request = call;
    cycle.reset();
    EXPECT_FALSE(owner.expired());  // a live request still keeps its owner
    call.reset();
    EXPECT_TRUE(request.expired());
    EXPECT_TRUE(owner.expired());
}

TEST(CoroDriverGatewayPhases, SavedServicesRejectHttpDuringCreateValidateDestroyAndAcceptOptionalErrors) {
    DriverCatalog catalog;
    NullLogger logger;
    ASSERT_NO_THROW(catalog.load_driver(PHASE_HTTP_FIXTURE));
    auto library = SharedLibrary::open(PHASE_HTTP_FIXTURE);
    ASSERT_TRUE(library);
    const auto destroy_status = reinterpret_cast<yaddnsc_status (*)()>(library->resolve("test_destroy_http_status"));
    ASSERT_NE(destroy_status, nullptr);
    LoopbackHttpServer server{0ms};
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::unique_ptr<plugin::DriverGateway>{},
            [&](coro::Nursery& group, std::unique_ptr<plugin::DriverGateway>& owner) -> coro::Task<void> {
                auto& gateway =
                    *(owner = std::make_unique<plugin::DriverGateway>(catalog, logger, loop, group, default_options()));
                {
                    const auto update = co_await gateway.update(
                        "phase_http", make_command(fmt::format("http://127.0.0.1:{}/", server.port())));
                    EXPECT_TRUE(update.has_value());
                    EXPECT_EQ(destroy_status(), YADDNSC_STATUS_INVALID_ARGUMENT);
                    const auto validation = co_await gateway.validate_config("phase_http", "{}");
                    EXPECT_TRUE(validation.has_value());
                    EXPECT_EQ(destroy_status(), YADDNSC_STATUS_INVALID_ARGUMENT);
                };
            });
    };
    coro::run(loop, root());
    EXPECT_EQ(server.request_count(), 3u);
}
