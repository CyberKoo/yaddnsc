//
// SchedulerRunner unit tests — the scheduling loop over a FakeClock and a
// recording FakeTaskExecutor; no real waiting, no provider.
//
// Locked behaviours:
//   - every due task is submitted on each round (initial deadline == now);
//   - pop-and-reschedule: advancing the fake clock by one interval
//     re-dispatches without any task finishing first;
//   - once stop is requested the runner never pops again and run() returns
//     promptly, even with a far-future deadline or an empty queue;
//   - one full application flow (queue → runner → executor → workflow over
//     mock ports) runs without any real provider.
//

#include "application/scheduler_runner.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <expected>
#include <glaze/glaze.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/ports/driver_gateway.h"
#include "application/ports/task_executor.h"
#include "application/update_workflow.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/schedule_queue.h"
#include "domain/update/time_types.h"
#include "domain/update/update_task.h"
#include "fixtures/sample_config.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations
#include "infrastructure/config/normalizer.h"
#include "mocks/fake_clock.h"
#include "mocks/fake_task_executor.h"
#include "mocks/mock_ports.h"
#include "mocks/null_logger.h"

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

const domain::TimePoint T0{std::chrono::seconds{10000}};

[[nodiscard]] std::shared_ptr<domain::RuntimeConfig> parse_cfg(std::string_view json) {
    Config::AppConfig raw;
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(raw, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return std::make_shared<domain::RuntimeConfig>(Config::normalize(raw));
}

// Single www.example.com A subdomain; force_update disabled (the DNS
// comparison path stays active), one 300s interval.
inline constexpr std::string_view FLOW_CONFIG = R"({
    "driver": { "auto_discover": true },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "example.com",
            "update_interval": 300,
            "force_update": 0,
            "driver": "cloudflare",
            "subdomains": [
                {"name": "www", "type": "a", "ip_source": "http",
                 "ip_source_param": "https://api.ipify.org"}
            ]
        }
    ]
})";

// Runs each submitted task inline on the runner thread — the full-flow test
// stays deterministic without a real thread pool.
class InlineTaskExecutor final : public TaskExecutor {
public:
    explicit InlineTaskExecutor(std::function<void(const domain::UpdateTask&)> fn) : fn_(std::move(fn)) {}

    bool submit(domain::UpdateTask task) override {
        if (shutdown_) {
            return false;
        }
        fn_(task);
        return true;
    }

    void wait_idle() override {}

    void shutdown() override { shutdown_ = true; }

    void set_retry_handler(RetryHandler) override {}

private:
    std::function<void(const domain::UpdateTask&)> fn_;
    bool shutdown_{false};
};

struct RunnerFixture {
    std::shared_ptr<domain::RuntimeConfig> config = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue{config, T0};
    FakeClock clock{T0};
    FakeTaskExecutor executor;
    NullLogger logger;
    std::stop_source stop;

    SchedulerRunner make_runner() { return {queue, clock, executor, stop.get_token(), logger}; }
};

}  // namespace

// Requests stop and joins the runner loop on destruction, so a failing
// assertion can never unwind past a joinable jthread (which would hang).
struct LoopGuard {
    std::stop_source& stop;
    std::jthread& loop;

    ~LoopGuard() {
        stop.request_stop();
        if (loop.joinable()) {
            loop.join();
        }
    }
};

// ── stop handling ────────────────────────────────────────────────────────────

TEST(SchedulerRunner, StopBeforeRunDispatchesNothing) {
    RunnerFixture f;
    f.stop.request_stop();

    auto runner = f.make_runner();
    runner.run();  // must return immediately

    EXPECT_TRUE(f.executor.submitted().empty());
}

TEST(SchedulerRunner, StopDuringWaitReturnsPromptly) {
    RunnerFixture f;
    auto runner = f.make_runner();

    std::jthread loop([&] { runner.run(); });
    const LoopGuard cleanup{f.stop, loop};
    ASSERT_TRUE(f.executor.wait_submitted(2));

    f.stop.request_stop();

    const auto start = std::chrono::steady_clock::now();
    // run() must return promptly although the next deadline is 300s of fake
    // time away (joining the jthread observes the return).
    loop.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);

    // No further dispatch after stop, even if time keeps advancing.
    f.clock.advance_by(600s);
    EXPECT_EQ(f.executor.submitted().size(), 2U);
}

TEST(SchedulerRunner, EmptyQueueWaitsOnlyForStop) {
    RunnerFixture f;
    f.config = parse_cfg(Fixtures::EMPTY_DOMAINS_CONFIG);
    f.queue = domain::ScheduleQueue(f.config, T0);
    auto runner = f.make_runner();

    std::atomic<bool> returned{false};
    std::jthread loop([&] {
        runner.run();
        returned.store(true);
    });
    const LoopGuard cleanup{f.stop, loop};

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(returned.load()) << "an empty queue must wait for stop, not spin or return";

    f.stop.request_stop();
    loop.join();
    EXPECT_TRUE(returned.load());
    EXPECT_TRUE(f.executor.submitted().empty());
}

// ── dispatch behaviour ───────────────────────────────────────────────────────

TEST(SchedulerRunner, InitialDispatchSubmitsEveryDueTaskForced) {
    RunnerFixture f;
    auto runner = f.make_runner();

    std::jthread loop([&] { runner.run(); });
    const LoopGuard cleanup{f.stop, loop};
    ASSERT_TRUE(f.executor.wait_submitted(2));
    f.stop.request_stop();
    loop.join();

    const auto tasks = f.executor.submitted();
    ASSERT_EQ(tasks.size(), 2U);
    // FULL_CONFIG has force_update=3600: the first pop of every task is forced.
    for (const auto& task : tasks) {
        EXPECT_TRUE(task.force_update);
    }

    std::vector<std::string> fqdns;
    for (const auto& task : tasks) {
        fqdns.push_back(task.fqdn);
    }
    EXPECT_THAT(fqdns, ::testing::UnorderedElementsAre("example.com", "www.example.com"));
}

TEST(SchedulerRunner, AdvancingTimeRedispatchesAfterInterval) {
    RunnerFixture f;
    auto runner = f.make_runner();

    std::jthread loop([&] { runner.run(); });
    const LoopGuard cleanup{f.stop, loop};
    ASSERT_TRUE(f.executor.wait_submitted(2));

    // Pop-and-reschedule: the previous tasks were never "finished" by the
    // fake executor, yet advancing one interval re-dispatches them.
    f.clock.advance_by(300s);
    ASSERT_TRUE(f.executor.wait_submitted(4));

    f.stop.request_stop();
    loop.join();

    const auto tasks = f.executor.submitted();
    ASSERT_EQ(tasks.size(), 4U);
    // Second round: 300s < force interval (3600s) — plain updates.
    EXPECT_FALSE(tasks[2].force_update);
    EXPECT_FALSE(tasks[3].force_update);
}

// ── retry_after rescheduling ─────────────────────────────────────────────────

// Spin until the runner has parked in wait_until for the n-th time, so
// stimuli land deterministically instead of racing the loop.
static bool wait_parked(const FakeClock& clock, unsigned n) {
    for (int i = 0; i < 30000; ++i) {
        if (clock.wait_entries() >= n) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

TEST(SchedulerRunner, RetryRequestMovesDeadlineAndWakesLoop) {
    RunnerFixture f;
    auto runner = f.make_runner();
    // Production wiring is RunLifecycle; here the test wires the handler
    // straight to the runner.
    f.executor.set_retry_handler(
        [&runner](domain::TaskId id, std::chrono::seconds delay) { runner.request_retry(id, delay); });

    std::jthread loop([&] { runner.run(); });
    const LoopGuard cleanup{f.stop, loop};

    ASSERT_TRUE(f.executor.wait_submitted(2));
    ASSERT_TRUE(wait_parked(f.clock, 1));

    // Rate-limit the first task ({0,0}) for two intervals; the wake must
    // pull the runner out of its 300s wait immediately.
    f.executor.fire_retry(domain::TaskId{0, 0}, 600s);
    ASSERT_TRUE(wait_parked(f.clock, 2));

    // One interval later only the second task may be re-dispatched.
    f.clock.advance_by(300s);
    ASSERT_TRUE(f.executor.wait_submitted(3));
    ASSERT_TRUE(wait_parked(f.clock, 3));
    EXPECT_EQ(f.executor.submitted().size(), 3U) << "the rate-limited task must not re-run at its old deadline";

    // Advancing the second interval reaches the moved deadline.
    f.clock.advance_by(300s);
    ASSERT_TRUE(f.executor.wait_submitted(4));

    f.stop.request_stop();
    loop.join();

    EXPECT_EQ(f.executor.submitted()[3].fqdn, "example.com");
}

// ── full application flow without a real provider ────────────────────────────

TEST(SchedulerRunner, FullUpdateCycleOverMockPorts) {
    const auto cfg = parse_cfg(FLOW_CONFIG);

    std::promise<void> first_update_done;
    std::promise<void> second_cycle_done;

    MockDnsResolverPort dns;
    MockIpSourcePort ip_source;
    MockDriverGateway gateway;
    NullLogger logger;

    EXPECT_CALL(ip_source, resolve(_))
        .WillRepeatedly(Return(std::vector<InetAddress>{InetAddress{Inet4Address::from_bytes({198, 51, 100, 1})}}));
    // Cycle 1: record differs from the local address → update. Cycle 2: the
    // record now matches → skip; the gateway must not be called again.
    EXPECT_CALL(dns, resolve("www.example.com", RecordKind::A))
        .WillOnce(Return(std::vector<std::string>{"192.0.2.1"}))
        .WillOnce([&second_cycle_done](std::string_view, RecordKind) {
            second_cycle_done.set_value();
            return std::expected<std::vector<std::string>, DnsErrorInfo>{{"198.51.100.1"}};
        });
    EXPECT_CALL(gateway, update("cloudflare", _))
        .WillOnce([&first_update_done](std::string_view,
                                       const DriverUpdateCommand& cmd) -> std::expected<void, domain::DriverError> {
            EXPECT_EQ(cmd.fqdn, "www.example.com");
            EXPECT_EQ(cmd.ip_addr, "198.51.100.1");
            first_update_done.set_value();
            return {};
        });

    const UpdateWorkflow workflow(dns, ip_source, gateway, logger);

    domain::ScheduleQueue queue(cfg, T0);
    FakeClock clock{T0};
    InlineTaskExecutor executor([&workflow](const domain::UpdateTask& task) { workflow.run(task); });

    std::stop_source stop;
    SchedulerRunner runner(queue, clock, executor, stop.get_token(), logger);
    std::jthread loop([&] { runner.run(); });
    const LoopGuard cleanup{stop, loop};

    ASSERT_EQ(first_update_done.get_future().wait_for(30s), std::future_status::ready)
        << "the initial cycle must update the changed record";

    clock.advance_by(300s);
    ASSERT_EQ(second_cycle_done.get_future().wait_for(30s), std::future_status::ready)
        << "advancing one interval must run the second cycle";

    stop.request_stop();
    loop.join();
    // gmock verifies: gateway called exactly once across both cycles.
}
