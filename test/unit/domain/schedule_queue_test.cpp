//
// ScheduleQueue unit tests — pure timer-queue logic over injected (fake)
// time; no clock, no threads, no stop tokens.
//
// Locked behaviours (legacy Scheduler semantics):
//   - one task per subdomain, first deadline == construction time
//   - pop-and-reschedule: a popped task is immediately re-queued with
//     now + update_interval (never waits for the task to finish)
//   - force_update > 0 → the FIRST pop of a task is forced
//   - force_update is a domain-level interval; update_interval is the
//     effective per-subdomain value (subdomain override wins)
//   - reschedule() moves the already-queued next deadline, never duplicates
//

#include "domain/update/schedule_queue.h"

#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include "domain/config/runtime_config.h"
#include "domain/update/time_types.h"
#include "domain/update/update_task.h"
#include "fixtures/sample_config.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations
#include "infrastructure/config/normalizer.h"

namespace {

using namespace std::chrono_literals;

// Fixed time base for the fake-time tests (arbitrary steady_clock value).
const domain::TimePoint T0{std::chrono::seconds{10000}};

// Parse one of the fixture configs and normalise it into a shared
// RuntimeConfig for the queue (no static validation: fixture intervals are
// deliberately below the production minimum).
[[nodiscard]] std::shared_ptr<domain::RuntimeConfig> parse_cfg(std::string_view json) {
    Config::AppConfig raw;
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(raw, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return std::make_shared<domain::RuntimeConfig>(Config::normalize(raw));
}

// Domain interval 600 with a subdomain override of 120 on "www".
inline constexpr std::string_view OVERRIDE_CONFIG = R"({
    "driver": { "auto_discover": true },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "test.com",
            "update_interval": 600,
            "force_update": 600,
            "driver": "cloudflare",
            "subdomains": [
                {"name": "www", "type": "a", "ip_source": "http",
                 "ip_source_param": "https://api.ipify.org", "update_interval": 120},
                {"name": "mail", "type": "a", "ip_source": "http",
                 "ip_source_param": "https://api.ipify.org"}
            ]
        }
    ]
})";

[[nodiscard]] bool contains_fqdn(const std::vector<domain::UpdateTask>& tasks, std::string_view fqdn) {
    for (const auto& task : tasks) {
        if (task.fqdn == fqdn) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ── Construction & initial population ────────────────────────────────────────

TEST(ScheduleQueue, InitialisesOneTaskPerSubdomain) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    const domain::ScheduleQueue queue(cfg, T0);

    EXPECT_EQ(queue.size(), 2U);
    EXPECT_FALSE(queue.empty());

    // Every entry's first deadline is the construction time.
    const auto due = domain::ScheduleQueue(cfg, T0).pop_due(T0);
    EXPECT_EQ(due.size(), 2U);
}

TEST(ScheduleQueue, ApexSubdomainUsesBareDomainFqdn) {
    // FULL_CONFIG's first subdomain is "@" (apex). The scheduled task must
    // look up example.com, not the literal "@.example.com".
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    const auto due = queue.pop_due(T0);
    ASSERT_EQ(due.size(), 2U);
    EXPECT_TRUE(contains_fqdn(due, "example.com"));
    EXPECT_TRUE(contains_fqdn(due, "www.example.com"));
}

TEST(ScheduleQueue, EmptyDomainListStaysEmpty) {
    const auto cfg = parse_cfg(Fixtures::EMPTY_DOMAINS_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_TRUE(queue.pop_due(T0).empty());
    EXPECT_FALSE(queue.time_until_next(T0).has_value());
}

// ── pop-and-reschedule ───────────────────────────────────────────────────────

TEST(ScheduleQueue, PopDueRequeuesTasksForNextCycleImmediately) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    const auto first = queue.pop_due(T0);
    ASSERT_EQ(first.size(), 2U);

    // Re-queued with a future deadline in the same call: a second pop at the
    // same instant returns nothing, and the queue is not empty.
    EXPECT_TRUE(queue.pop_due(T0).empty());
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.time_until_next(T0), std::optional{std::chrono::seconds{300}});
}

TEST(ScheduleQueue, PopAtNextDeadlineDispatchesAgain) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    ASSERT_EQ(queue.pop_due(T0).size(), 2U);
    const auto second = queue.pop_due(T0 + 300s);
    ASSERT_EQ(second.size(), 2U);
    for (const auto& task : second) {
        EXPECT_FALSE(task.force_update) << "re-queued cycle must not be forced (interval 300 < force 3600)";
    }
}

TEST(ScheduleQueue, DueTasksComeOutEarliestDeadlineFirst) {
    const auto cfg = parse_cfg(OVERRIDE_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    // Both tasks are due at T0; ties keep config order.
    auto due = queue.pop_due(T0);
    ASSERT_EQ(due.size(), 2U);
    EXPECT_EQ(due[0].fqdn, "www.test.com");
    EXPECT_EQ(due[1].fqdn, "mail.test.com");

    // www re-queued at +120 (subdomain override), mail at +600 (domain value).
    EXPECT_EQ(queue.time_until_next(T0), std::optional{120s});

    due = queue.pop_due(T0 + 120s);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].fqdn, "www.test.com");

    // At +600 both are due; www's pending deadline (240) precedes mail's (600).
    due = queue.pop_due(T0 + 600s);
    ASSERT_EQ(due.size(), 2U);
    EXPECT_EQ(due[0].fqdn, "www.test.com");
    EXPECT_EQ(due[1].fqdn, "mail.test.com");
}

TEST(ScheduleQueue, TimeUntilNextClampsAtZeroForMissedDeadlines) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    ASSERT_EQ(queue.pop_due(T0).size(), 2U);
    // No pop happened at +305: the deadline (T0+300) is already in the past.
    EXPECT_EQ(queue.time_until_next(T0 + 305s), std::optional{0s});
}

// ── force_update ─────────────────────────────────────────────────────────────

TEST(ScheduleQueue, FirstPopIsForcedWhenForceUpdateEnabled) {
    // FULL_CONFIG has force_update=3600: last_force_update starts one full
    // interval in the past, so the very first pop is forced.
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    const auto due = queue.pop_due(T0);
    ASSERT_EQ(due.size(), 2U);
    for (const auto& task : due) {
        EXPECT_TRUE(task.force_update);
    }
}

TEST(ScheduleQueue, ForceUpdateTriggersAgainAfterIntervalElapses) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    ASSERT_EQ(queue.pop_due(T0).size(), 2U);         // forced, last_force = T0
    ASSERT_EQ(queue.pop_due(T0 + 300s).size(), 2U);  // 300 < 3600: not forced
    const auto third = queue.pop_due(T0 + 3600s);    // 3600 >= 3600: forced again
    ASSERT_EQ(third.size(), 2U);
    for (const auto& task : third) {
        EXPECT_TRUE(task.force_update);
    }
}

TEST(ScheduleQueue, NoForceUpdateWhenIntervalIsZero) {
    const auto cfg = parse_cfg(Fixtures::NO_FORCE_UPDATE_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    for (const auto at : {T0, T0 + 300s, T0 + 86400s}) {
        for (const auto& task : queue.pop_due(at)) {
            EXPECT_FALSE(task.force_update) << "force_update must stay false when the interval is 0";
        }
    }
}

TEST(ScheduleQueue, DomainForceIntervalWithSubdomainIntervalOverride) {
    // OVERRIDE_CONFIG: force_update=600 on the domain; www overrides the
    // update interval to 120, mail inherits 600.
    const auto cfg = parse_cfg(OVERRIDE_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    ASSERT_EQ(queue.pop_due(T0).size(), 2U);  // both forced (first pop)

    auto due = queue.pop_due(T0 + 120s);  // only www due; 120 < 600: not forced
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].fqdn, "www.test.com");
    EXPECT_FALSE(due[0].force_update);

    // At +600 both are due and the domain-level force interval has elapsed
    // for both (last force was T0).
    due = queue.pop_due(T0 + 600s);
    ASSERT_EQ(due.size(), 2U);
    for (const auto& task : due) {
        EXPECT_TRUE(task.force_update);
    }
    // www still re-queues on its 120s override.
    EXPECT_EQ(queue.time_until_next(T0 + 600s), std::optional{120s});
}

// ── invalid intervals ────────────────────────────────────────────────────────

TEST(ScheduleQueue, ZeroUpdateIntervalThrows) {
    // A domain with update_interval = 0 and no subdomain override would
    // re-queue the entry with deadline == now forever (busy loop). The
    // constructor must reject it instead of relying on static validation.
    const auto json = R"({
        "driver": { "auto_discover": true },
        "resolver": { "use_custom_server": false },
        "domains": [
            {
                "name": "test.com",
                "update_interval": 0,
                "driver": "cloudflare",
                "subdomains": [
                    {"name": "www", "type": "a", "ip_source": "http",
                     "ip_source_param": "https://api.ipify.org"}
                ]
            }
        ]
    })";
    const auto cfg = parse_cfg(json);
    try {
        const domain::ScheduleQueue queue(cfg, T0);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_STREQ(e.what(), "Update interval for www.test.com must be positive (got 0)");
    }
}

// ── reschedule ───────────────────────────────────────────────────────────────

TEST(ScheduleQueue, RescheduleMovesTheQueuedDeadlineWithoutDuplicating) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    ASSERT_EQ(queue.pop_due(T0).size(), 2U);  // both re-queued at T0+300

    // Move www's next deadline closer; the entry count must not change.
    EXPECT_TRUE(queue.reschedule(domain::TaskId{0, 1}, T0 + 30s));
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.time_until_next(T0), std::optional{30s});

    const auto due = queue.pop_due(T0 + 30s);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].fqdn, "www.example.com");

    // www re-queued at 30+300; the apex task is still waiting for T0+300.
    EXPECT_EQ(queue.time_until_next(T0 + 30s), std::optional{270s});
}

TEST(ScheduleQueue, RescheduleBeforeFirstPopSuppressesImmediateDispatch) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    EXPECT_TRUE(queue.reschedule(domain::TaskId{0, 0}, T0 + 1000s));
    const auto due = queue.pop_due(T0);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].fqdn, "www.example.com");
}

TEST(ScheduleQueue, RescheduleUnknownTaskIdReturnsFalse) {
    const auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    domain::ScheduleQueue queue(cfg, T0);

    EXPECT_FALSE(queue.reschedule(domain::TaskId{9, 9}, T0));
    EXPECT_EQ(queue.time_until_next(T0), std::optional{0s});  // untouched
}
