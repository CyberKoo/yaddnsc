//
// Component test: Manager lifecycle — locks the observable shutdown sequence:
//
//   stop signal
//   → scheduler stops dispatching new tasks
//   → in-flight task finishes (or is cancelled) before run() returns
//   → thread pool is drained before the driver module can be unloaded
//
// Uses a fake DNS resolver (fixed wire response), a blocking fake HttpClient
// and the real "simple" driver .so loaded through the production dlopen path.
// No real provider or network access is involved; the fake client intercepts
// every exchange before any socket is opened.
//
// Per refactor/phase-0-baseline.md §0.5 this test intentionally does NOT use
// a fake clock: the initial deadline is "now", the rescheduled deadline is one
// hour out, and blocking latches — not sleeps — order the two threads.
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <glaze/glaze.hpp>

#include "core/manager.h"

#include "config/config.h"
#include "config/normalizer.h"
#include "config/parser.hpp"
#include "dns/dispatcher.h"
#include "interface/http_client.h"
#include "ip_source/iface.h"
#include "ip_source/iface_util.h"

#include "mocks/mock_resolver.h"

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

// ── Fake DNS: one A record (192.0.2.1) for any query ─────────────────────────

class FixedAResolver : public MockResolver {
public:
    FixedAResolver() {
        ON_CALL(*this, query(_, _))
            .WillByDefault(Return(std::vector<std::uint8_t>{
                0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
                0x00, 0x01, 0x00, 0x01, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x01, 0x2C, 0x00, 0x04, 0xC0, 0x00, 0x02, 0x01}));
        ON_CALL(*this, get_type()).WillByDefault(Return("Mock"));
    }
};

[[nodiscard]] ResolverDispatcher make_dispatcher() {
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::make_unique<FixedAResolver>());
    return ResolverDispatcher(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);
}

// ── Fake HTTP: blocks inside exchange() until the test releases it ───────────

struct BlockingHttpState {
    std::mutex mtx;
    std::condition_variable cv;
    bool entered{false};   // an exchange() call is parked
    bool released{false};  // the test allows the parked call to return
    std::atomic<int> calls{0};
    std::atomic<bool> completed{false};  // set after a parked exchange returns

    void wait_entered() {
        std::unique_lock lock(mtx);
        const bool ok = cv.wait_for(lock, 10s, [this] { return entered; });
        EXPECT_TRUE(ok) << "no HTTP exchange started — the initial task was never dispatched";
    }

    void release() {
        std::lock_guard lock(mtx);
        released = true;
        cv.notify_all();
    }
};

class BlockingHttpClient : public HttpClient {
public:
    explicit BlockingHttpClient(std::shared_ptr<BlockingHttpState> state) : state_(std::move(state)) {}

    std::expected<net::http::Response, net::http::Error> exchange(std::string_view /*url*/,
                                                                  const net::http::Request & /*req*/) const override {
        state_->calls.fetch_add(1);
        {
            std::unique_lock lock(state_->mtx);
            state_->entered = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [this] { return state_->released; });
        }
        state_->completed.store(true);
        return net::http::Response{200, "ok", {}};
    }

private:
    std::shared_ptr<BlockingHttpState> state_;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

// Any interface that yields at least one IPv4 address ("lo" on Linux, "lo0" on
// macOS). The loopback address differs from the fake DNS record (192.0.2.1),
// so the updater takes the update path and invokes the driver.
[[nodiscard]] std::optional<std::string> find_ipv4_interface() {
    for (const auto &name: InterfaceUtil::get_interfaces()) {
        InterfaceIpSource source(name, AddressFamily::IPV4);
        if (!source.resolve().empty()) {
            return name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] domain::RuntimeConfig parse_cfg(const std::string &json) {
    Config::AppConfig cfg{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return Config::normalize(cfg);
}

// One subdomain on the real "simple" driver; update_interval is one hour, so
// the only prompt dispatch is the initial one (first deadline == now). The
// driver's URL template is intercepted by the fake HttpClient before any
// socket is opened.
[[nodiscard]] std::string lifecycle_config(std::string_view interface_name) {
    return std::string(R"({"driver":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
           R"(","load":["simple/simple.so"]},"resolver":{"use_custom_server":false},)" +
           R"("domains":[{"name":"example.com","update_interval":3600,"force_update":0,"driver":"simple",)" +
           R"("subdomains":[{"name":"www","type":"a","ip_source":"interface","interface":")" +
           std::string(interface_name) +
           R"(","driver_param":{"url":"http://127.0.0.1/update?ip={ip_addr}"}}]}]})";
}

} // namespace

// stop while one task is blocked in HTTP: run() must keep waiting until the
// in-flight task finishes, return promptly afterwards (the next deadline is an
// hour away), and never dispatch again.
TEST(ManagerLifecycle, StopDrainsInFlightTaskBeforeReturning) {
    const auto interface_name = find_ipv4_interface();
    if (!interface_name) {
        GTEST_SKIP() << "no interface with an IPv4 address on this host";
    }

    std::stop_source stop_source;
    auto state = std::make_shared<BlockingHttpState>();
    HttpClientFactory http_factory = [state] { return std::make_unique<BlockingHttpClient>(state); };

    Manager manager(parse_cfg(lifecycle_config(*interface_name)), stop_source, make_dispatcher(), http_factory);
    manager.load_drivers();

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        manager.run();
        run_done.set_value();
    });

    // First task dispatched immediately (initial deadline == now) and parked.
    state->wait_entered();

    stop_source.request_stop();

    // Draining, not abandoning: run() must not return while the in-flight
    // task is still blocked inside the driver call.
    EXPECT_EQ(run_future.wait_for(200ms), std::future_status::timeout)
        << "run() returned before the in-flight task finished";

    state->release();

    // stop interrupts the scheduler wait: run() returns promptly although the
    // rescheduled deadline is one hour out.
    ASSERT_EQ(run_future.wait_for(10s), std::future_status::ready)
        << "run() did not return after stop + drain";

    // Exactly one dispatch happened — after stop, the re-queued entry never ran.
    EXPECT_EQ(state->calls.load(), 1);
    // The in-flight task ran to completion before run() returned.
    EXPECT_TRUE(state->completed.load());

    // ~Manager() now unloads the driver module after the pool has drained;
    // ASan/UBSan flag any use-after-dlclose in this ordering.
}

// stop requested before run(): no task is ever dispatched.
TEST(ManagerLifecycle, StopBeforeRunDispatchesNothing) {
    const auto interface_name = find_ipv4_interface();
    if (!interface_name) {
        GTEST_SKIP() << "no interface with an IPv4 address on this host";
    }

    std::stop_source stop_source;
    auto state = std::make_shared<BlockingHttpState>();
    HttpClientFactory http_factory = [state] { return std::make_unique<BlockingHttpClient>(state); };

    Manager manager(parse_cfg(lifecycle_config(*interface_name)), stop_source, make_dispatcher(), http_factory);
    manager.load_drivers();

    stop_source.request_stop();

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        manager.run();
        run_done.set_value();
    });

    ASSERT_EQ(run_future.wait_for(10s), std::future_status::ready) << "run() blocked despite a pre-requested stop";
    EXPECT_EQ(state->calls.load(), 0);
}
