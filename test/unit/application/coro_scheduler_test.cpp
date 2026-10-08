//
// Coroutine scheduler tests — the per-subdomain scheduling coroutine and the run
// root.
//
// Timeline behaviour is deterministic: the loop runs on a ManualClock, so
// "sleep for the update interval" really advances simulated time. The run-root
// supervisor/SIGINT test uses the system clock with a 1s interval.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; coroutine bodies use EXPECT_* only.
//

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <csignal>
#include <expected>
#include <gtest/gtest.h>
#include <unistd.h>

#include "application/run_scheduler.h"
#include "application/services.h"
#include "application/subdomain_loop.h"
#include "application/update_once.h"
#include "application/ports/log.h"
#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

[[nodiscard]] InetAddress address(std::string_view literal) {
    const auto parsed = InetAddress::parse(literal);
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(InetAddress{});
}

/// Logger double: every level disabled, so the YLOG_* macros format nothing.
class NullLogger final : public Logger {
public:
    [[nodiscard]] bool is_enabled(LogLevel) const override { return false; }
    void log(LogLevel, std::string_view, const std::source_location&) const override {}
};

class FakeResolver final : public app::ResolverPort {
public:
    std::vector<std::string> records{"198.51.100.1"};
    /// When positive, resolve() parks this long (a resolver that never answers).
    std::chrono::seconds hang{0};
    int calls = 0;

    coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> resolve(std::string, RecordKind) override {
        ++calls;
        if (hang.count() > 0) {
            const auto slept = co_await coro::sleep_for(hang);
            if (!slept.has_value()) {
                co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "test resolver cancelled"});
            }
        }
        co_return records;
    }
};

class FakeIpSource final : public app::IpSourcePort {
public:
    std::vector<InetAddress> addresses{address("192.0.2.1")};
    /// Subdomain names for which resolve() throws std::bad_alloc (a defect).
    std::vector<std::string> fatal_subdomains;
    int calls = 0;

    coro::Task<std::expected<std::vector<InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) override {
        ++calls;
        for (const auto& name : fatal_subdomains) {
            if (name == config.name) {
                throw std::bad_alloc{};
            }
        }
        co_return addresses;
    }
};

class FakeGateway final : public app::GatewayPort {
public:
    int calls = 0;
    /// Fail the first call with this retry_after (0 = never fail).
    int fail_first_retry_after = 0;
    /// Invoked on every call, after the counters are updated.
    std::function<void()> on_call;

    coro::Task<std::expected<void, domain::DriverError>> update(std::string, domain::DriverUpdateCommand command) override {
        ++calls;
        fqdns.push_back(command.fqdn);
        if (on_call) {
            on_call();
        }
        if (fail_first_retry_after > 0) {
            const int retry_after = fail_first_retry_after;
            fail_first_retry_after = 0;
            co_return std::unexpected(
                domain::DriverError{domain::DriverError::Code::RATE_LIMITED, "slow down", retry_after});
        }
        co_return std::expected<void, domain::DriverError>{};
    }

    std::vector<std::string> fqdns;
};

[[nodiscard]] domain::SubdomainConfig make_subdomain(std::string name, int interval) {
    domain::SubdomainConfig subdomain;
    subdomain.name = std::move(name);
    subdomain.type = RecordKind::A;
    subdomain.ip_source = Config::IpSource::INTERFACE;
    subdomain.interface = "lo";
    subdomain.update_interval = interval;
    return subdomain;
}

/// One domain holding the given subdomains.
[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> make_config(std::vector<domain::SubdomainConfig> subdomains,
                                                                      int interval, int force_update) {
    domain::RuntimeConfig config;
    config.resolver.servers.push_back(Config::DnsServer{"1.1.1.1", 53});
    domain::DomainConfig domain;
    domain.name = "example.com";
    domain.update_interval = interval;
    domain.force_update = force_update;
    domain.driver = "fake-driver";
    domain.subdomains = std::move(subdomains);
    config.domains.push_back(std::move(domain));
    return std::make_shared<const domain::RuntimeConfig>(std::move(config));
}

/// Run a task-factory lambda on `loop` (coro::run takes an already-built Task).
template<typename Fn>
void run_loop(coro::Loop& loop, Fn&& body) {
    coro::run(loop, body());
}

}  // namespace

// ---------------------------------------------------------------------------
// subdomain_loop timing (ManualClock)
// ---------------------------------------------------------------------------

TEST(CoroScheduler, SubdomainLoopRunsOnTheUpdateInterval) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(25s, [&](coro::CancelScope&) -> coro::Task<void> {
            co_await app::subdomain_loop(config, 0, 0, services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0, t=10, t=20; the t=25 budget cancels the sleep towards t=30.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 3);
}

TEST(CoroScheduler, RetryAfterOverridesTheUpdateInterval) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.fail_first_retry_after = 30;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(45s, [&](coro::CancelScope&) -> coro::Task<void> {
            co_await app::subdomain_loop(config, 0, 0, services);
            co_return;
        });
        co_return;
    });

    // t=0 fails with retry_after=30, so the next attempt is at t=30, then t=40.
    EXPECT_EQ(gateway.calls, 3);
}

TEST(CoroScheduler, ForceUpdateIntervalSkipsTheDnsRead) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 15);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(25s, [&](coro::CancelScope&) -> coro::Task<void> {
            co_await app::subdomain_loop(config, 0, 0, services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0 (forced), t=10, t=20 (forced at 20 >= 15): two forced
    // cycles skip the DNS read, so only the t=10 cycle consults the resolver.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(CoroScheduler, CancellationWakesTheLoopFromItsSleep) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(5s, [&](coro::CancelScope&) -> coro::Task<void> {
            co_await app::subdomain_loop(config, 0, 0, services);
            co_return;
        });
        co_return;
    });

    // One cycle at t=0; the t=5 budget cancels the 100s sleep, so the loop exits
    // without a second cycle.
    EXPECT_EQ(gateway.calls, 1);
}

// ---------------------------------------------------------------------------
// update_once decisions
// ---------------------------------------------------------------------------

TEST(CoroScheduler, UpdateOnceSkipsWhenTheRecordIsUnchanged) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::SKIP_UNCHANGED);
    EXPECT_EQ(gateway.calls, 0);
}

TEST(CoroScheduler, UpdateOnceUpdatesWhenTheRecordChanged) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    ASSERT_EQ(gateway.fqdns.size(), 1u);
    EXPECT_EQ(gateway.fqdns[0], "www.example.com");
}

TEST(CoroScheduler, UpdateOnceForceSkipsTheResolver) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", true};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_FORCED);
    EXPECT_EQ(resolver.calls, 0);
    EXPECT_EQ(gateway.calls, 1);
}

TEST(CoroScheduler, UpdateOncePropagatesDriverRetryAfter) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.fail_first_retry_after = 42;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_EQ(outcome.error().retry_after_seconds, 42);
}

TEST(CoroScheduler, UpdateOnceSkipsWhenNoLocalAddress) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.addresses.clear();
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(gateway.calls, 0);
}

// ---------------------------------------------------------------------------
// DNS read budget (timeout is a scope property composed in the workflow)
// ---------------------------------------------------------------------------

TEST(CoroScheduler, UpdateOnceDnsReadTimeoutStillPublishes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.hang = 3600s;  // never answers within the read budget
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};
    const auto start = clock.now();

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::update_once(task, services);
        co_return;
    });

    // The read's own budget fired: it is a transient failure, so the update
    // still goes out (empty record list decides UPDATE_CHANGED).
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    EXPECT_GE(clock.now() - start, std::chrono::seconds(4));
}

TEST(CoroScheduler, UpdateOnceExternalCancelDoesNotPublish) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.hang = 3600s;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config, 0, 0, "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        // The outer scope fires at 2s, before the 4s read budget: an external
        // cancel, not our timeout, must abort without publishing.
        co_await coro::with_timeout(2s, [&](coro::CancelScope&) -> coro::Task<void> {
            outcome = co_await app::update_once(task, services);
            co_return;
        });
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::CANCELLED);
    EXPECT_EQ(gateway.calls, 0);
}

// ---------------------------------------------------------------------------
// next_delay
// ---------------------------------------------------------------------------

TEST(CoroScheduler, NextDelayHonoursRetryAfterAndInterval) {
    const app::UpdateOnceOutcome failure{
        std::unexpect, domain::UpdateError{domain::UpdateError::Code::DRIVER_FAILED, "x", 42}};
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(app::next_delay(failure, 10)).count(), 42);

    const app::UpdateOnceOutcome plain_failure{
        std::unexpect, domain::UpdateError{domain::UpdateError::Code::DRIVER_FAILED, "x", 0}};
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(app::next_delay(plain_failure, 10)).count(), 10);

    const app::UpdateOnceOutcome success{app::UpdateCycleResult{domain::UpdateDecision::UPDATE_CHANGED}};
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(app::next_delay(success, 7)).count(), 7);
}

// ---------------------------------------------------------------------------
// run_scheduler: supervisor semantics and SIGINT shutdown
// ---------------------------------------------------------------------------

TEST(CoroScheduler, RunRootSupervisesSubdomainsAndStopsOnSigint) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.push_back("bad");
    FakeGateway gateway;
    // The second successful cycle asks the process to shut down, which proves
    // the healthy subdomain kept running after its sibling died.
    gateway.on_call = [&gateway] {
        if (gateway.calls >= 2) {
            ::kill(::getpid(), SIGINT);
        }
    };
    NullLogger logger;

    const auto config = make_config({make_subdomain("bad", 1), make_subdomain("good", 1)}, 1, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway](coro::TaskGroup&) -> app::GatewayPort& { return gateway; },
    };

    const int code = coro::run(loop, app::run_scheduler(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    // The healthy subdomain updated at least twice; the failing one never did.
    EXPECT_GE(gateway.calls, 2);
    for (const auto& fqdn : gateway.fqdns) {
        EXPECT_EQ(fqdn, "good.example.com");
    }
    EXPECT_FALSE(resolver.calls == 0);
}
