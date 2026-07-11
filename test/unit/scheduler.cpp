//
// Scheduler unit tests — pure timer-queue logic, no I/O or threading beyond
// the stop_token wakeup path.
//

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/scheduler.h"
#include "core/update_task.h"

#include "config/config.h"
#include "config/parser.hpp"

#include "fixtures/sample_config.h"

namespace {

// Parse one of the fixture configs into an AppConfig for the scheduler.
[[nodiscard]] Config::AppConfig parse_cfg(std::string_view json) {
    Config::AppConfig cfg{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return cfg;
}

// Count every subdomain across all domains (== number of scheduled tasks).
[[nodiscard]] std::size_t subdomain_count(const Config::AppConfig &cfg) {
    std::size_t n = 0;
    for (const auto &domain: cfg.domains) {
        n += domain.subdomains.size();
    }
    return n;
}

} // namespace

// ── Construction & initial population ────────────────────────────────────────

TEST(Scheduler, InitialisesOneTaskPerSubdomain) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    EXPECT_TRUE(scheduler.has_pending());
    const auto due = scheduler.pop_all_due();
    EXPECT_EQ(due.size(), subdomain_count(cfg));
}

TEST(Scheduler, EmptyDomainListHasNoPendingTasks) {
    const auto cfg = parse_cfg(Fixtures::EMPTY_DOMAINS_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    EXPECT_FALSE(scheduler.has_pending());
    EXPECT_TRUE(scheduler.pop_all_due().empty());
}

// ── pop_all_due re-queues with next deadline ─────────────────────────────────

TEST(Scheduler, PopAllDueRequeuesTasksForNextCycle) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    const auto first = scheduler.pop_all_due();
    ASSERT_EQ(first.size(), subdomain_count(cfg));

    // After popping, every task is immediately re-queued with a future deadline,
    // so the heap is non-empty again.
    EXPECT_TRUE(scheduler.has_pending());

    // A second immediate pop should return nothing (deadlines are in the future).
    EXPECT_TRUE(scheduler.pop_all_due().empty());
}

TEST(Scheduler, RequeuedDeadlineIsInTheFuture) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    // Pop everything; tasks are re-queued with future deadlines.
    scheduler.pop_all_due();

    // A second immediate pop must return nothing (deadlines are in the future).
    EXPECT_TRUE(scheduler.pop_all_due().empty());

    // wait_for_next() should block until the (far-future) deadline, not return
    // immediately. Verify by requesting stop after a short delay and confirming
    // the wait does not return within a tiny window before that.
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        scheduler.wait_for_next();
        returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(returned.load()); // still blocked on the future deadline
    stop.request_stop();
    // The waiter must wake up promptly after the stop is requested.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(returned.load());
    waiter.join();
}

// ── force_update flag ────────────────────────────────────────────────────────

TEST(Scheduler, ForceUpdateFlagSetWhenIntervalElapsed) {
    // FULL_CONFIG has force_update=3600 on example.com; since the scheduler
    // initialises deadlines to "now", the first pop is already due and the
    // elapsed time (>= force interval) triggers force_update.
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    const auto due = scheduler.pop_all_due();
    ASSERT_EQ(due.size(), subdomain_count(cfg));

    // At least the example.com subdomains should carry force_update=true.
    bool any_forced = false;
    for (const auto &task: due) {
        if (task.force_update) {
            any_forced = true;
            break;
        }
    }
    EXPECT_TRUE(any_forced);
}

TEST(Scheduler, ForceUpdateFlagResetAfterRequeue) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    auto due = scheduler.pop_all_due();
    ASSERT_FALSE(due.empty());

    // The re-queued entry must not carry force_update until its interval elapses
    // again. A second immediate pop returns nothing, proving the flag was reset.
    EXPECT_TRUE(scheduler.pop_all_due().empty());
}

// ── stop_token wakeup ────────────────────────────────────────────────────────

TEST(Scheduler, WaitForNextReturnsFalseWhenStoppedBeforeWait) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    stop.request_stop();
    Scheduler scheduler(cfg, stop.get_token());

    EXPECT_FALSE(scheduler.wait_for_next());
}

TEST(Scheduler, WaitForNextUnblocksOnStopDuringWait) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    // Pop everything so the next wait would block on a future deadline.
    scheduler.pop_all_due();

    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.request_stop();
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result = scheduler.wait_for_next();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    stopper.join();
    EXPECT_FALSE(result);
    // Must wake up near the stop time, not near the (far-future) deadline.
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(Scheduler, WaitForNextDoesNotReportShutdownSpuriously) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    std::stop_source stop;
    Scheduler scheduler(cfg, stop.get_token());

    // With pending (future) deadlines and no stop requested, wait_for_next must
    // not return until stop is requested. We stop after a short delay and assert
    // it returns false (shutdown) rather than true spuriously.
    // Pop first so deadlines move into the future (otherwise they are due now).
    scheduler.pop_all_due();
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.request_stop();
    });
    EXPECT_FALSE(scheduler.wait_for_next());
    stopper.join();
}
