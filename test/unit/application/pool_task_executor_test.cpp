//
// PoolTaskExecutor unit tests — the thread-pool-backed TaskExecutor adapter
// (refactor/phase-3-scheduling-and-workflow.md §3.5).
//
// Locked behaviours:
//   - submitted tasks run the UpdateWorkflow on pool threads;
//   - shutdown() rejects new submissions; in-flight tasks are unaffected;
//   - wait_idle() blocks until every accepted task has finished (drain
//     before the driver gateway's modules may be unloaded);
//   - tasks of the SAME module (shared C++ Driver behind the gateway) still
//     run concurrently — the Phase 2–3 concurrency contract.
//

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/pool_task_executor.h"
#include "application/update_workflow.h"

#include "domain/fqdn.h"
#include "domain/update/update_task.h"
#include "network/inet_address.h"

#include "config/config.h"
#include "config/normalizer.h"
#include "config/parser.hpp"

#include "fixtures/sample_config.h"
#include "mocks/mock_ports.h"
#include "mocks/null_logger.h"

namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> parse_cfg(std::string_view json) {
    Config::AppConfig raw;
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(raw, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return std::make_shared<const domain::RuntimeConfig>(Config::normalize(raw));
}

[[nodiscard]] domain::UpdateTask make_task(const std::shared_ptr<const domain::RuntimeConfig> &cfg,
                                           std::size_t sub_idx) {
    const auto &domain = cfg->domains[0];
    const auto &sub = domain.subdomains[sub_idx];
    return domain::UpdateTask{
        .config = cfg,
        .domain_index = 0,
        .subdomain_index = sub_idx,
        .fqdn = domain::make_fqdn(domain.name, sub.name),
        .force_update = true, // skip the DNS read: these tests target the executor
    };
}

// IP answers matching the fixture subdomains ("@" is type A, "www" AAAA).
void stub_ip_answers(MockIpSourcePort &ip_source) {
    ON_CALL(ip_source, resolve(_))
        .WillByDefault([](const domain::SubdomainConfig &sub) {
            if (sub.type == RecordKind::AAAA) {
                return std::expected<std::vector<InetAddress>, domain::IpSourceError>{
                    {InetAddress{Inet6Address::from_bytes(
                        {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})}}};
            }
            return std::expected<std::vector<InetAddress>, domain::IpSourceError>{
                {InetAddress{Inet4Address::from_bytes({198, 51, 100, 1})}}};
        });
}

struct Fixture {
    std::shared_ptr<const domain::RuntimeConfig> config = parse_cfg(Fixtures::FULL_CONFIG);
    MockDnsResolverPort dns;
    MockIpSourcePort ip_source;
    MockDriverGateway gateway;
    NullLogger logger;

    Fixture() { stub_ip_answers(ip_source); }

    UpdateWorkflow make_workflow() { return {dns, ip_source, gateway, logger}; }
};

} // namespace

// ── submitted tasks actually run ─────────────────────────────────────────────

TEST(PoolTaskExecutor, RunsSubmittedTaskToCompletion) {
    Fixture f;
    std::promise<void> done;
    EXPECT_CALL(f.gateway, update("cloudflare", _))
        .WillOnce([&done](std::string_view, const DriverUpdateCommand &) -> std::expected<void, domain::DriverError> {
            done.set_value();
            return {};
        });

    auto workflow = f.make_workflow();
    PoolTaskExecutor executor(2, workflow);

    ASSERT_TRUE(executor.submit(make_task(f.config, 0)));
    executor.wait_idle();
    EXPECT_EQ(done.get_future().wait_for(0s), std::future_status::ready);
}

// ── shutdown rejects new tasks ───────────────────────────────────────────────

TEST(PoolTaskExecutor, RejectsTasksAfterShutdown) {
    Fixture f;
    EXPECT_CALL(f.gateway, update(_, _)).Times(0);

    auto workflow = f.make_workflow();
    PoolTaskExecutor executor(2, workflow);
    executor.shutdown();

    EXPECT_FALSE(executor.submit(make_task(f.config, 0)));
    executor.wait_idle();
}

// ── wait_idle drains in-flight tasks ─────────────────────────────────────────

TEST(PoolTaskExecutor, WaitIdleBlocksUntilInFlightTaskFinishes) {
    Fixture f;

    std::mutex mtx;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    EXPECT_CALL(f.gateway, update(_, _))
        .WillOnce([&](std::string_view, const DriverUpdateCommand &) -> std::expected<void, domain::DriverError> {
            std::unique_lock lock(mtx);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
            return {};
        });

    auto workflow = f.make_workflow();
    PoolTaskExecutor executor(2, workflow);
    ASSERT_TRUE(executor.submit(make_task(f.config, 0)));

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return entered; })) << "task never started";
    }

    std::atomic<bool> drained{false};
    std::jthread drainer([&] {
        executor.wait_idle();
        drained.store(true);
    });

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(drained.load()) << "wait_idle returned while the task was still blocked";

    {
        std::lock_guard lock(mtx);
        released = true;
        cv.notify_all();
    }
    drainer.join();
    EXPECT_TRUE(drained.load());
}

// ── same-module tasks still run concurrently (Phase 2–3 contract) ────────────

TEST(PoolTaskExecutor, SameModuleTasksRunConcurrently) {
    Fixture f;

    // Barrier: each gateway call blocks until BOTH tasks are inside update().
    // If the pool serialized same-module tasks this would deadlock (and the
    // 5s timeout would fail the test).
    std::mutex mtx;
    std::condition_variable cv;
    int inside = 0;
    std::atomic<int> calls{0};

    EXPECT_CALL(f.gateway, update("cloudflare", _))
        .Times(2)
        .WillRepeatedly([&](std::string_view, const DriverUpdateCommand &)
                            -> std::expected<void, domain::DriverError> {
            calls.fetch_add(1);
            std::unique_lock lock(mtx);
            ++inside;
            cv.notify_all();
            const bool overlapped = cv.wait_for(lock, 5s, [&] { return inside == 2; });
            EXPECT_TRUE(overlapped) << "same-module tasks did not run concurrently";
            return {};
        });

    auto workflow = f.make_workflow();
    PoolTaskExecutor executor(2, workflow);

    ASSERT_TRUE(executor.submit(make_task(f.config, 0))); // "@"  (A)
    ASSERT_TRUE(executor.submit(make_task(f.config, 1))); // "www" (AAAA)
    executor.wait_idle();

    EXPECT_EQ(calls.load(), 2);
}
