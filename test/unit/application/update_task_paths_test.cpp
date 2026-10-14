//
// update_task path tests — the error-mapping arms of one update cycle.
//
// main_loop_test.cpp covers the decision table (unchanged / changed / forced)
// and the two timeout budgets. This file covers what happens when a *port*
// reports a failure instead: a source that cannot answer, a resolver that
// fails, the three driver-error codes, and the three exception arms. Every arm
// here decides something a reader has to get right — which update cycle error
// comes out, whether the update is published anyway, and which log level the
// driver failure is reported at.
//

#include <chrono>
#include <functional>
#include <new>
#include <optional>
#include <stdexcept>
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
#include "application/tasks/update_task.h"
#include "coro/coro.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "mocks/recording_logger.h"

namespace {

using namespace std::chrono_literals;
using SourceResult = std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>;
using LookupResult = std::expected<std::vector<std::string>, domain::DnsErrorInfo>;

[[nodiscard]] domain::InetAddress address(std::string_view literal) {
    const auto parsed = domain::InetAddress::parse(literal);
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(domain::InetAddress{});
}

class ScriptedResolver final : public app::ResolverPort {
public:
    /// When set, resolve() reports this error instead of the records.
    std::optional<domain::DnsErrorInfo> error;
    std::vector<std::string> records{"198.51.100.1"};
    int calls = 0;

    [[nodiscard]] coro::Task<LookupResult> resolve(std::string, domain::RecordKind) override {
        ++calls;
        if (error.has_value()) {
            co_return std::unexpected(*error);
        }
        co_return records;
    }
};

class ScriptedIpSource final : public app::IpSourcePort {
public:
    std::optional<domain::IpSourceError> error;
    std::vector<domain::InetAddress> addresses{address("192.0.2.1")};
    int calls = 0;

    [[nodiscard]] coro::Task<SourceResult> resolve(const domain::SubdomainConfig&) override {
        ++calls;
        if (error.has_value()) {
            co_return std::unexpected(*error);
        }
        co_return addresses;
    }
};

/// Gateway double whose update() can answer with each driver-error code or
/// throw; `throw_value` is thrown as-is so the coroutine's own exception arms
/// decide what happens.
class ScriptedGateway final : public app::GatewayPort {
public:
    std::optional<domain::DriverError> failure;
    std::function<void()> throw_value;
    std::vector<std::string> published;

    [[nodiscard]] bool supports_validate(std::string_view) const override { return true; }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string,
                                                                                       std::string) override {
        co_return std::expected<void, domain::DriverError>{};
    }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(
        std::string, domain::DriverUpdateCommand cmd) override {
        published.push_back(cmd.fqdn);
        if (throw_value) {
            throw_value();
        }
        if (failure.has_value()) {
            co_return std::unexpected(*failure);
        }
        co_return std::expected<void, domain::DriverError>{};
    }
};

[[nodiscard]] domain::SubdomainConfig make_subdomain() {
    domain::SubdomainConfig value;
    value.name = "www";
    value.type = domain::RecordKind::A;
    value.ip_source = domain::IpSource::INTERFACE;
    value.interface = "lo";
    value.update_interval = 10;
    return value;
}

[[nodiscard]] domain::DomainConfig make_domain() {
    domain::DomainConfig value;
    value.name = "example.com";
    value.driver = "fake-driver";
    value.update_interval = 10;
    value.subdomains = {make_subdomain()};
    return value;
}

/// domain::UpdateTask *borrows* its domain and subdomain configuration, so the
/// owner of both has to outlive every cycle that runs it. This bundle keeps the
/// two configuration values and the task built over them together; a cycle may
/// only run while the bundle is alive.
class Cycle {
public:
    Cycle() : domain_config(make_domain()), subdomain_config(make_subdomain()) {}

    [[nodiscard]] domain::UpdateTask task(bool force) const {
        return domain::UpdateTask{
            domain_config,
            subdomain_config,
            domain::make_fqdn(domain_config.name, subdomain_config.name),
            force,
        };
    }

    domain::DomainConfig domain_config;
    domain::SubdomainConfig subdomain_config;
};

/// Run one cycle to completion and hand back its outcome.
[[nodiscard]] app::UpdateCycleOutcome run_cycle(const domain::UpdateTask& task, const app::Services& services) {
    coro::Loop loop;
    app::UpdateCycleOutcome outcome;
    const auto body = [&]() -> coro::Task<void> {
        outcome = co_await app::update_task(task, services);
        co_return;
    };
    coro::run(loop, body());
    return outcome;
}

/// True when any record at `level` mentions every fragment.
[[nodiscard]] bool logged(const RecordingLogger& logger, app::LogLevel level,
                          const std::vector<std::string>& fragments) {
    for (const auto& record : logger.records()) {
        if (record.level != level) {
            continue;
        }
        bool all = true;
        for (const auto& fragment : fragments) {
            all = all && record.message.find(fragment) != std::string::npos;
        }
        if (all) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Step 1: the local-IP source reports a failure
// ---------------------------------------------------------------------------

TEST(UpdateTaskPaths, SourceErrorSkipsTheCycleWithItsOwnMessage) {
    // A source that cannot answer is a transient skip, not a driver failure:
    // the cycle ends before the DNS read and before the gateway.
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ip_source.error = domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, "no route to host"};
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(outcome.error().message, "no route to host");
    EXPECT_EQ(resolver.calls, 0);
    EXPECT_TRUE(gateway.published.empty());
    EXPECT_TRUE(logged(logger, app::LogLevel::WARN,
                       {"Failed to resolve the local IP address", "no route to host", "www.example.com"}));
}

// ---------------------------------------------------------------------------
// Step 2: the DNS read reports a failure — the update still goes out
// ---------------------------------------------------------------------------

TEST(UpdateTaskPaths, ResolverFailureStillPublishes) {
    // Pushing an unchanged record is harmless, skipping a changed one is not:
    // an unreadable current record must not cancel the update.
    ScriptedResolver resolver;
    resolver.error = domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "no such name"};
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    ASSERT_EQ(gateway.published.size(), 1u);
    EXPECT_EQ(gateway.published.front(), "www.example.com");
    EXPECT_TRUE(logged(logger, app::LogLevel::WARN,
                       {"DNS lookup for \"www.example.com\" failed", "no such name", "proceeding with update"}));
}

// ---------------------------------------------------------------------------
// Step 4: the three driver-error codes each get their own wording and level
// ---------------------------------------------------------------------------

TEST(UpdateTaskPaths, DriverNotFoundIsAnErrorNamingTheRecord) {
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.failure = domain::DriverError{domain::DriverError::Code::NOT_FOUND, "driver not loaded", 0};
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_EQ(outcome.error().message, "driver not loaded");
    EXPECT_TRUE(
        logged(logger, app::LogLevel::ERROR, {"driver not loaded", "Skipping the update for \"www.example.com\""}));
    // A missing driver is not a backoff condition.
    EXPECT_EQ(outcome.error().retry_after_seconds, 0);
}

TEST(UpdateTaskPaths, DriverUnknownIsReportedAsAnUnhandledException) {
    // UNKNOWN is the driver's catch-all for a defect inside the plugin, so it
    // keeps the legacy "unhandled exception" wording.
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.failure = domain::DriverError{domain::DriverError::Code::UNKNOWN, "bad parameter json", 0};
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_TRUE(logged(logger, app::LogLevel::ERROR,
                       {"Unhandled exception during update of \"www.example.com\"", "bad parameter json"}));
}

TEST(UpdateTaskPaths, DriverTransportFailureIsOnlyDebugLogged) {
    // The driver already logged its own upstream/HTTP failure, so the host
    // repeats it at DEBUG instead of raising a second ERROR for one event.
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.failure = domain::DriverError{domain::DriverError::Code::UPDATE_FAILED, "upstream said 503", 11};
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    // The backoff the driver asked for survives the mapping.
    EXPECT_EQ(outcome.error().retry_after_seconds, 11);
    EXPECT_TRUE(logged(logger, app::LogLevel::DEBUG, {"upstream said 503"}));
    EXPECT_FALSE(logged(logger, app::LogLevel::ERROR, {"upstream said 503"}));
}

// ---------------------------------------------------------------------------
// Exception arms: a defect is downgraded, an allocation failure is not
// ---------------------------------------------------------------------------

TEST(UpdateTaskPaths, GatewayThrowingStdExceptionBecomesAnUnknownError) {
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.throw_value = [] { throw std::runtime_error("plugin segfaulted"); };
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::UNKNOWN);
    EXPECT_EQ(outcome.error().message, "plugin segfaulted");
    EXPECT_TRUE(logged(logger, app::LogLevel::ERROR,
                       {"Unhandled exception during update of \"www.example.com\"", "plugin segfaulted"}));
}

TEST(UpdateTaskPaths, GatewayThrowingNonStandardBecomesAGenericUnknownError) {
    // A driver that throws something the host cannot describe still ends the
    // cycle as a value, with the legacy wording that names no detail.
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.throw_value = [] { throw 42; };
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);
    const auto outcome = run_cycle(task, services);

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::UNKNOWN);
    EXPECT_EQ(outcome.error().message, "Unknown non-standard exception");
    EXPECT_TRUE(
        logged(logger, app::LogLevel::ERROR, {"Unknown non-standard exception during update for \"www.example.com\""}));
}

TEST(UpdateTaskPaths, AllocationFailureFromTheGatewayPropagates) {
    // bad_alloc is a defect, not a retryable condition: it must never be
    // downgraded into an UpdateError value.
    ScriptedResolver resolver;
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    gateway.throw_value = [] { throw std::bad_alloc{}; };
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(false);

    EXPECT_THROW({ [[maybe_unused]] const auto outcome = run_cycle(task, services); }, std::bad_alloc);
}

TEST(UpdateTaskPaths, ForcedCycleSkipsTheReadAndReportsTheForce) {
    // force_update bypasses the DNS read entirely and reports the forced
    // decision, whatever the current record says.
    ScriptedResolver resolver;
    resolver.records = {"192.0.2.1"};  // identical to the local address
    ScriptedIpSource ip_source;
    ScriptedGateway gateway;
    RecordingLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};

    const Cycle cycle;
    const domain::UpdateTask task = cycle.task(true);
    const auto outcome = run_cycle(task, services);

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_FORCED);
    EXPECT_EQ(resolver.calls, 0);
    ASSERT_EQ(gateway.published.size(), 1u);
    EXPECT_TRUE(logged(logger, app::LogLevel::INFO, {"Force update triggered for \"www.example.com\""}));
}
