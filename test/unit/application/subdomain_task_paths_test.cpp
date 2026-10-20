//
// subdomain_task path tests — the pacing policy around one scheduled record.
//
// main_loop_test.cpp covers the ordinary interval/retry_after/force cadence.
// This file covers the inputs the existing cases never produce: a
// configuration that skipped static validation (a non-positive update
// interval, which the task has to clamp itself), a force interval that
// lands exactly on the elapsed time (the `>=` boundary of the due check), a
// timer that wakes late, and a clock near its representable limit. All decide
// *when* the next cycle runs, so all are worth pinning down.
//

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — required by coroutine lowering

#include <expected>
#include <gtest/gtest.h>

#include "application/ports/gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/log.h"
#include "application/ports/resolver.h"
#include "application/services.h"
#include "application/tasks/subdomain_task.h"
#include "coro/coro.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "mocks/recording_logger.h"

#include "min_update_interval.h"

namespace {

using namespace std::chrono_literals;

[[nodiscard]] domain::InetAddress address(std::string_view literal) {
    const auto parsed = domain::InetAddress::parse(literal);
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(domain::InetAddress{});
}

class ScriptedResolver final : public app::ResolverPort {
public:
    std::vector<std::string> records{"198.51.100.1"};
    int calls = 0;

    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string, domain::RecordKind) override {
        ++calls;
        co_return records;
    }
};

class ScriptedIpSource final : public app::IpSourcePort {
public:
    std::vector<domain::InetAddress> addresses{address("192.0.2.1")};
    int calls = 0;
    /// When set, the loop time at which each cycle consulted the source.
    const coro::Clock* clock = nullptr;
    std::vector<coro::TimePoint> resolved_at;

    [[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig&) override {
        ++calls;
        if (clock != nullptr) {
            resolved_at.push_back(clock->now());
        }
        co_return addresses;
    }
};

class ScriptedGateway final : public app::GatewayPort {
public:
    int calls = 0;

    [[nodiscard]] bool supports_validate(std::string_view) const override { return true; }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string,
                                                                                       std::string) override {
        co_return std::expected<void, domain::DriverError>{};
    }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(std::string,
                                                                              domain::DriverUpdateCommand) override {
        ++calls;
        co_return std::expected<void, domain::DriverError>{};
    }
};

/// Manual clock whose every loop-driven jump lands `lateness` past the
/// requested deadline, the way a real timer wakes up a little late.
class LateWakeClock final : public coro::Clock {
public:
    explicit LateWakeClock(coro::Duration lateness) noexcept : lateness_(lateness) {}

    [[nodiscard]] coro::TimePoint now() const noexcept override { return now_; }

    [[nodiscard]] bool manual() const noexcept override { return true; }

    void jump_to(coro::TimePoint tp) noexcept override {
        if (tp > now_) {
            now_ = tp + lateness_;
        }
    }

private:
    coro::Duration lateness_;
    coro::TimePoint now_{};
};

[[nodiscard]] domain::DomainConfig make_domain(int force_interval, int update_interval = 0) {
    domain::DomainConfig value;
    value.name = "example.com";
    value.driver = "fake-driver";
    value.update_interval = update_interval;
    value.force_update = force_interval;
    value.subdomains.push_back(domain::SubdomainConfig{.update_interval = update_interval});
    return value;
}

/// Run subdomain_task for `budget` of simulated time, then let it go.
void drive(coro::Loop& loop, const domain::DomainConfig& domain_config, const app::Services& services,
           coro::Duration budget) {
    const auto body = [&]() -> coro::Task<void> {
        co_await coro::with_timeout(budget, [&]() -> coro::Task<void> {
            co_await app::subdomain_task(domain_config, domain_config.subdomains.front(), services);
            co_return;
        });
        co_return;
    };
    coro::run(loop, body());
}

}  // namespace

TEST(SubdomainTaskPaths, NonPositiveIntervalIsClampedToTheMinimum) {
    // A configuration that never went through static validation can reach the
    // scheduler with interval 0. Sleeping 0 would spin the loop, so the task
    // clamps to the built-in minimum and says so.
    coro::ManualClock clock;
    coro::Loop loop{clock};
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const domain::DomainConfig domain_config = make_domain(0);

    drive(loop, domain_config, services, std::chrono::seconds(YADDNSC_MIN_UPDATE_INTERVAL * 2 + 5));

    // Cycles at t=0, t=60, t=120 — the clamped interval, not the configured 0.
    EXPECT_EQ(ip_source.calls, 3);
    EXPECT_EQ(gateway.calls, 3);

    bool warned = false;
    for (const auto& record : logger.records()) {
        if (record.level == app::LogLevel::WARN &&
            record.message.find("non-positive update interval") != std::string::npos &&
            record.message.find("clamping") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(SubdomainTaskPaths, ClampedIntervalStillPacesTheForcedCycles) {
    // The clamp must not be a one-off warning: it has to bound the wait the
    // task actually takes between a forced publish and the next cycle.
    coro::ManualClock clock;
    coro::Loop loop{clock};
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    // force_update == the clamped interval: every cycle is a forced publish,
    // so the resolver is never consulted and the gap is the clamped one.
    const domain::DomainConfig domain_config = make_domain(YADDNSC_MIN_UPDATE_INTERVAL);

    drive(loop, domain_config, services, std::chrono::seconds(YADDNSC_MIN_UPDATE_INTERVAL * 2 + 5));

    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 0);
}

TEST(SubdomainTaskPaths, ForceIntervalDueExactlyOnTheElapsedTime) {
    // The due check is `elapsed >= force_interval`, so a cycle that starts
    // exactly one interval after the last publish is forced again. Cycles at
    // t=0 and t=20 are forced; t=10 is not.
    coro::ManualClock clock;
    coro::Loop loop{clock};
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const domain::DomainConfig domain_config = make_domain(20, 10);

    drive(loop, domain_config, services, 25s);

    EXPECT_EQ(gateway.calls, 3);
    // Only the unforced cycle at t=10 reads the current record.
    EXPECT_EQ(resolver.calls, 1);
}

TEST(SubdomainTaskPaths, LateTimerWakeUpsDoNotAccumulateIntoTheCadence) {
    // Every wake-up lands 7ms after its deadline. The plain interval runs from
    // the scheduled cycle start, so cycle k still starts at k*10s + 7ms rather
    // than drifting by another 7ms per cycle.
    constexpr auto LATENESS = 7ms;
    LateWakeClock clock{LATENESS};
    coro::Loop loop{clock};
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ip_source.clock = &clock;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const domain::DomainConfig domain_config = make_domain(0, 10);

    drive(loop, domain_config, services, 55s);

    ASSERT_EQ(ip_source.resolved_at.size(), 6U);
    EXPECT_EQ(ip_source.resolved_at.front(), coro::TimePoint{});
    for (std::size_t cycle = 1; cycle < ip_source.resolved_at.size(); ++cycle) {
        const auto scheduled = std::chrono::seconds(10) * static_cast<int>(cycle);
        EXPECT_EQ(ip_source.resolved_at[cycle] - coro::TimePoint{}, scheduled + LATENESS) << "cycle " << cycle;
    }
}

TEST(SubdomainTaskPaths, UnrepresentableNextDeadlineStopsTheTask) {
    coro::ManualClock clock{coro::TimePoint::max() - 5s};
    coro::Loop loop{clock};
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const domain::DomainConfig domain_config = make_domain(0, 10);

    drive(loop, domain_config, services, 1s);

    EXPECT_EQ(ip_source.calls, 1);
    EXPECT_EQ(gateway.calls, 1);

    bool reported = false;
    for (const auto& record : logger.records()) {
        if (record.level == app::LogLevel::ERROR && record.message.find("timer range") != std::string::npos) {
            reported = true;
        }
    }
    EXPECT_TRUE(reported);
}
