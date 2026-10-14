//
// subdomain_task path tests — the pacing policy around one scheduled record.
//
// main_loop_test.cpp covers the ordinary interval/retry_after/force cadence.
// This file covers the two inputs the existing cases never produce: a
// configuration that skipped static validation (a non-positive update
// interval, which the task has to clamp itself) and a force interval that
// lands exactly on the elapsed time (the `>=` boundary of the due check).
// Both decide *when* the next cycle runs, so both are worth pinning down.
//

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <coroutine>

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

    [[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig&) override {
        ++calls;
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
