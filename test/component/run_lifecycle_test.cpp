//
// Component test: RunLifecycle — locks the observable shutdown sequence:
//
//   stop signal
//   → scheduler stops dispatching new tasks
//   → in-flight task finishes (or is cancelled) before run() returns
//   → thread pool is drained before the driver module can be unloaded
//
// Two levels of doubles:
//   - FakeClock / FakeTaskExecutor / NullLogger / fake interfaces: the stop →
//     cancel-I/O → shutdown → drain sequence without any real I/O;
//   - the full run graph (real "simple" driver .so through the production
//     dlopen path, fake DNS with a fixed wire response, blocking fake
//     HttpClient): drain ordering with real plugins, exactly as the
//     composition root wires it. No real provider or network access.
//
// The full-graph tests intentionally do NOT use a fake clock: the initial
// deadline is "now", the rescheduled deadline is one hour out, and blocking
// latches — not sleeps — order the threads.
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

#include "application/pool_task_executor.h"
#include "application/run_lifecycle.h"
#include "application/update_workflow.h"
#include "infrastructure/plugin/driver_loader.h"
#include "infrastructure/logging/spdlog_logger.h"
#include "infrastructure/time/steady_clock.h"
#include "infrastructure/plugin/abi_driver_gateway.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/network/system_network_interfaces.h"

#include "infrastructure/config/config.h"
#include "infrastructure/config/normalizer.h"
#include "infrastructure/config/parser.hpp"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/network/http/client_port.h"
#include "infrastructure/ip_source/iface.h"
#include "infrastructure/ip_source/iface_util.h"

#include "support/util/cancellation_token.hpp"

#include "mocks/fake_clock.h"
#include "mocks/fake_task_executor.h"
#include "mocks/mock_ports.h"
#include "mocks/mock_resolver.h"
#include "mocks/null_logger.h"

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

// Requests stop and joins the runner thread on destruction, so a failing
// assertion can never unwind past a joinable jthread (which would hang).
struct RunnerGuard {
    std::stop_source &stop;
    std::jthread &runner;
    ~RunnerGuard() {
        stop.request_stop();
        if (runner.joinable()) {
            runner.join();
        }
    }
};

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

/// The run graph, assembled exactly as the composition root does: catalog +
/// loader, gateway over the injected HttpClient factory, workflow, executor.
/// Construction order is the member declaration order (config first; the
/// cancellation source before every token consumer).
struct RunGraph {
    RunGraph(domain::RuntimeConfig config, ResolverDispatcher dispatcher, HttpClientFactory http_factory)
        : config_(std::make_shared<const domain::RuntimeConfig>(std::move(config))),
          dispatcher_(std::move(dispatcher)),
          ip_source_(cancellation_.token()),
          gateway_(catalog_, std::move(http_factory), cancellation_.token(), logger_),
          workflow_(dispatcher_, ip_source_, gateway_, logger_),
          executor_(2, workflow_) {
        DriverLoader::load(catalog_, config_->driver);
    }

    std::shared_ptr<const domain::RuntimeConfig> config_;
    Utils::CancellationSource cancellation_;
    SpdlogLogger logger_;
    DriverCatalog catalog_;
    SteadyClock clock_;
    SystemNetworkInterfaces interfaces_;
    ResolverDispatcher dispatcher_;
    IpSourceAdapter ip_source_;
    AbiDriverGateway gateway_;
    UpdateWorkflow workflow_;
    PoolTaskExecutor executor_;
};

/// Minimal runtime config for the fake-graph tests: one domain, one
/// subdomain, a one-hour interval (only the initial "now" dispatch fires).
[[nodiscard]] domain::RuntimeConfig fake_graph_config() {
    domain::RuntimeConfig config;
    config.domains.push_back(domain::DomainConfig{
        .name = "example.com",
        .update_interval = 3600,
        .force_update = 0,
        .driver = "any",
        .subdomains = {{
            domain::SubdomainConfig{
                .name = "www",
                .type = RecordKind::A,
                .ip_source = Config::IpSource::HTTP,
                .ip_source_param = "https://api.ipify.org",
                .update_interval = 3600,
            },
        }},
    });
    return config;
}

const domain::TimePoint T0{std::chrono::seconds{10000}};

/// Spin until the fake clock has been entered `n` times — i.e. the scheduling
/// loop has consumed the previous stimulus and parked again — so a test can
/// inject a retry only after the runner has applied the last one (removes the
/// advance-vs-reschedule race).
[[nodiscard]] bool wait_parked(const FakeClock &clock, unsigned n) {
    for (int i = 0; i < 5000; ++i) {
        if (clock.wait_entries() >= n) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

// stop while one task is blocked in HTTP: run() must keep waiting until the
// in-flight task finishes, return promptly afterwards (the next deadline is an
// hour away), and never dispatch again.
TEST(RunLifecycle, StopDrainsInFlightTaskBeforeReturning) {
    const auto interface_name = find_ipv4_interface();
    if (!interface_name) {
        GTEST_SKIP() << "no interface with an IPv4 address on this host";
    }

    std::stop_source stop_source;
    auto state = std::make_shared<BlockingHttpState>();
    HttpClientFactory http_factory = [state] { return std::make_unique<BlockingHttpClient>(state); };

    RunGraph graph(parse_cfg(lifecycle_config(*interface_name)), make_dispatcher(), http_factory);
    RunLifecycle lifecycle(graph.config_, stop_source, graph.cancellation_, graph.clock_, graph.executor_,
                           graph.interfaces_, graph.logger_);

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        lifecycle.run();
        run_done.set_value();
    });
    const RunnerGuard guard{stop_source, runner};

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

    // Destroying the graph now unloads the driver module after the pool has
    // drained; ASan/UBSan flag any use-after-dlclose in this ordering.
}

// stop requested before run(): no task is ever dispatched.
TEST(RunLifecycle, StopBeforeRunDispatchesNothing) {
    const auto interface_name = find_ipv4_interface();
    if (!interface_name) {
        GTEST_SKIP() << "no interface with an IPv4 address on this host";
    }

    std::stop_source stop_source;
    auto state = std::make_shared<BlockingHttpState>();
    HttpClientFactory http_factory = [state] { return std::make_unique<BlockingHttpClient>(state); };

    RunGraph graph(parse_cfg(lifecycle_config(*interface_name)), make_dispatcher(), http_factory);
    RunLifecycle lifecycle(graph.config_, stop_source, graph.cancellation_, graph.clock_, graph.executor_,
                           graph.interfaces_, graph.logger_);

    stop_source.request_stop();

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        lifecycle.run();
        run_done.set_value();
    });
    const RunnerGuard guard{stop_source, runner};

    ASSERT_EQ(run_future.wait_for(10s), std::future_status::ready) << "run() blocked despite a pre-requested stop";
    EXPECT_EQ(state->calls.load(), 0);
}

// stop → I/O cancellation fires, the executor is shut down and drained, and
// the runner never dispatches again — observed through port fakes plus the
// real cancellation primitive.
TEST(RunLifecycle, StopCancelsIoAndDrainsExecutor) {
    auto config = std::make_shared<const domain::RuntimeConfig>(fake_graph_config());
    Utils::CancellationSource cancellation;
    NullLogger logger;
    FakeClock clock{T0};
    FakeTaskExecutor executor;
    MockNetworkInterfaces interfaces;
    ON_CALL(interfaces, names()).WillByDefault(Return(std::vector<std::string>{"lo"}));

    std::stop_source stop_source;
    RunLifecycle lifecycle(config, stop_source, cancellation, clock, executor, interfaces, logger);

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        lifecycle.run();
        run_done.set_value();
    });
    const RunnerGuard guard{stop_source, runner};

    // Initial deadline == now: the first task is dispatched immediately, then
    // the runner parks on the fake clock one hour out.
    ASSERT_TRUE(executor.wait_submitted(1));

    EXPECT_FALSE(cancellation.is_triggered());
    stop_source.request_stop();
    EXPECT_TRUE(cancellation.is_triggered());

    ASSERT_EQ(run_future.wait_for(10s), std::future_status::ready) << "run() did not return after stop";
    // The executor was shut down (rejecting further submits) and drained once.
    EXPECT_TRUE(executor.is_shutdown());
    EXPECT_EQ(executor.wait_idle_calls(), 1);
    // The one-hour-out reschedule never dispatched.
    EXPECT_EQ(executor.submitted().size(), 1);
}

// A stop requested before run() still triggers I/O cancellation immediately
// (the binding is established at construction).
TEST(RunLifecycle, PreStopCancelsIoBeforeRun) {
    auto config = std::make_shared<const domain::RuntimeConfig>(fake_graph_config());
    Utils::CancellationSource cancellation;
    NullLogger logger;
    FakeClock clock{T0};
    FakeTaskExecutor executor;
    MockNetworkInterfaces interfaces;
    ON_CALL(interfaces, names()).WillByDefault(Return(std::vector<std::string>{"lo"}));

    std::stop_source stop_source;
    RunLifecycle lifecycle(config, stop_source, cancellation, clock, executor, interfaces, logger);

    stop_source.request_stop();
    EXPECT_TRUE(cancellation.is_triggered());

    lifecycle.run();
    EXPECT_TRUE(executor.is_shutdown());
    EXPECT_TRUE(executor.submitted().empty());
}

// A rate-limit report for an in-flight task reaches the scheduler runner
// through the retry handler the lifecycle installs at construction: the
// runner moves the task's deadline to now + retry_after and re-dispatches it
// there, not at the one-hour update interval.
TEST(RunLifecycle, RateLimitedTaskIsRescheduledAtRetryDeadline) {
    auto config = std::make_shared<const domain::RuntimeConfig>(fake_graph_config());
    Utils::CancellationSource cancellation;
    NullLogger logger;
    FakeClock clock{T0};
    FakeTaskExecutor executor;
    MockNetworkInterfaces interfaces;
    ON_CALL(interfaces, names()).WillByDefault(Return(std::vector<std::string>{"lo"}));

    std::stop_source stop_source;
    RunLifecycle lifecycle(config, stop_source, cancellation, clock, executor, interfaces, logger);

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    std::jthread runner([&] {
        lifecycle.run();
        run_done.set_value();
    });
    const RunnerGuard guard{stop_source, runner};

    // Initial deadline == now: the first task is dispatched immediately, then
    // the runner parks one hour out.
    ASSERT_TRUE(executor.wait_submitted(1));
    ASSERT_TRUE(wait_parked(clock, 1));

    // Rate-limit the task for 30s. fire_retry() invokes the handler exactly
    // as a pool thread would; the runner applies the retry and re-parks.
    executor.fire_retry(domain::TaskId{0, 0}, 30s);
    ASSERT_TRUE(wait_parked(clock, 2));

    // The one-hour deadline is ignored; at now + 30s the task re-dispatches.
    clock.advance_to(T0 + 30s);
    ASSERT_TRUE(executor.wait_submitted(2)) << "task was not re-dispatched at the retry deadline";

    stop_source.request_stop();
    ASSERT_EQ(run_future.wait_for(10s), std::future_status::ready) << "run() did not return after stop";
}
