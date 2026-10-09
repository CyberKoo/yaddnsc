//
// Run-root tests — the per-subdomain loop coroutine and the run root.
//
// Timeline behaviour is deterministic: the loop runs on a ManualClock, so
// "sleep for the update interval" really advances simulated time. The run-root
// supervisor/SIGINT test uses the system clock with a 1s interval.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; coroutine bodies use EXPECT_* only.
//

#include "application/run_root.h"

#include <atomic>
#include <chrono>
#include <csignal>
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

#include <expected>
#include <gtest/gtest.h>
#include <unistd.h>

#include "application/ports/log.h"
#include "application/services.h"
#include "application/subdomain_loop.h"
#include "application/run_update_cycle.h"
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
#include "mocks/null_logger.h"
#include "mocks/recording_logger.h"

namespace {

using namespace std::chrono_literals;

[[nodiscard]] domain::InetAddress address(std::string_view literal) {
    const auto parsed = domain::InetAddress::parse(literal);
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(domain::InetAddress{});
}

class FakeResolver final : public app::ResolverPort {
public:
    std::vector<std::string> records{"198.51.100.1"};
    /// When positive, resolve() parks this long (a resolver that never answers).
    std::chrono::seconds hang{0};
    int calls = 0;

    coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(std::string,
                                                                                      domain::RecordKind) override {
        ++calls;
        if (hang.count() > 0) {
            co_await coro::sleep_for(hang);
        }
        co_return records;
    }
};

class FakeIpSource final : public app::IpSourcePort {
public:
    std::vector<domain::InetAddress> addresses{address("192.0.2.1")};
    /// Subdomain names for which resolve() throws std::bad_alloc (a defect).
    std::vector<std::string> fatal_subdomains;
    /// When positive, resolve() parks this long (a source that never answers).
    std::chrono::seconds hang{0};
    int calls = 0;
    /// Called just before a successful result completes, while its parent is parked.
    std::function<void()> on_resolved;

    coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) override {
        ++calls;
        for (const auto& name : fatal_subdomains) {
            if (name == config.name) {
                throw std::bad_alloc{};
            }
        }
        if (hang.count() > 0) {
            co_await coro::sleep_for(hang);
        }
        if (on_resolved) {
            on_resolved();
        }
        co_return addresses;
    }
};

class FakeGateway final : public app::GatewayPort {
public:
    int calls = 0;
    /// Fail the first call with this retry_after (0 = never fail).
    int fail_first_retry_after = 0;
    /// When positive, update() parks this long (a driver that never returns).
    std::chrono::seconds hang{0};
    /// Invoked on every call, after the counters are updated.
    std::function<void()> on_call;

    coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string, std::string) override {
        co_return std::expected<void, domain::DriverError>{};
    }

    coro::Task<std::expected<void, domain::DriverError>> update(std::string,
                                                                domain::DriverUpdateCommand command) override {
        ++calls;
        fqdns.push_back(command.fqdn);
        if (on_call) {
            on_call();
        }
        if (hang.count() > 0) {
            co_await coro::sleep_for(hang);
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

/// A gateway whose bookkeeping lives in shared state. The run root owns the
/// gateway (unique_ptr factory) and destroys it when the run ends, so a test
/// that drives run_root inspects this shared state instead of the object.
class ProbeGateway final : public app::GatewayPort {
public:
    struct State {
        int calls = 0;
        std::vector<std::string> fqdns;
        /// Invoked on every call, after the counters are updated.
        std::function<void()> on_call;
    };

    explicit ProbeGateway(std::shared_ptr<State> state) : state_(std::move(state)) {}

    coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string, std::string) override {
        co_return std::expected<void, domain::DriverError>{};
    }

    coro::Task<std::expected<void, domain::DriverError>> update(std::string,
                                                                domain::DriverUpdateCommand command) override {
        ++state_->calls;
        state_->fqdns.push_back(command.fqdn);
        if (state_->on_call) {
            state_->on_call();
        }
        co_return std::expected<void, domain::DriverError>{};
    }

private:
    std::shared_ptr<State> state_;
};

[[nodiscard]] domain::SubdomainConfig make_subdomain(std::string name, int interval) {
    domain::SubdomainConfig subdomain;
    subdomain.name = std::move(name);
    subdomain.type = domain::RecordKind::A;
    subdomain.ip_source = domain::IpSource::INTERFACE;
    subdomain.interface = "lo";
    subdomain.update_interval = interval;
    return subdomain;
}

/// One domain holding the given subdomains.
[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> make_config(std::vector<domain::SubdomainConfig> subdomains,
                                                                       int interval, int force_update) {
    domain::RuntimeConfig config;
    config.resolver.servers.push_back(domain::DnsServer{"1.1.1.1", 53});
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

TEST(RunRoot, SubdomainLoopRunsOnTheUpdateInterval) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(25s, [&]() -> coro::Task<void> {
            co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0, t=10, t=20; the t=25 budget cancels the sleep towards t=30.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 3);
}

TEST(RunRoot, RetryAfterOverridesTheUpdateInterval) {
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
        co_await coro::with_timeout(45s, [&]() -> coro::Task<void> {
            co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // t=0 fails with retry_after=30, so the next attempt is at t=30, then t=40.
    EXPECT_EQ(gateway.calls, 3);
}

TEST(RunRoot, ForceUpdateIntervalSkipsTheDnsRead) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 15);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(25s, [&]() -> coro::Task<void> {
            co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0 (forced), t=10, t=20 (forced at 20 >= 15): two forced
    // cycles skip the DNS read, so only the t=10 cycle consults the resolver.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(RunRoot, CancellationWakesTheLoopFromItsSleep) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(5s, [&]() -> coro::Task<void> {
            co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // One cycle at t=0; the t=5 budget cancels the 100s sleep, so the loop exits
    // without a second cycle.
    EXPECT_EQ(gateway.calls, 1);
}

TEST(RunRoot, SubdomainLoop_AlreadyCancelled_DoesNotResolveDnsOrPublish) {
    // Cover both ordinary DNS verification and forced publishing.
    for (const int force_interval : {0, 60}) {
        coro::ManualClock clock;
        coro::Loop loop{clock};
        FakeResolver resolver;
        FakeIpSource ip_source;
        FakeGateway gateway;
        NullLogger logger;
        const app::Services services{resolver, ip_source, gateway, logger};
        const auto config = make_config({make_subdomain("www", 10)}, 10, force_interval);

        run_loop(loop, [&]() -> coro::Task<void> {
            co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                scope.cancel();
                co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            });
        });

        // Awaiting a Task starts its body even in a cancelled scope. The source
        // result's scope outcome stops the cycle; no later port is entered.
        EXPECT_EQ(ip_source.calls, 1);
        EXPECT_EQ(resolver.calls, 0);
        EXPECT_EQ(gateway.calls, 0);
    }
}

TEST(RunRoot, SubdomainLoop_SourceCompletedBeforeCancel_DoesNotResolveDnsOrPublish) {
    // Cover both ordinary DNS verification and forced publishing.
    for (const int force_interval : {0, 60}) {
        coro::ManualClock clock;
        coro::Loop loop{clock};
        FakeResolver resolver;
        FakeIpSource ip_source;
        FakeGateway gateway;
        NullLogger logger;
        const app::Services services{resolver, ip_source, gateway, logger};
        const auto config = make_config({make_subdomain("www", 10)}, 10, force_interval);
        bool cancellation_delivered = false;

        run_loop(loop, [&]() -> coro::Task<void> {
            co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                ip_source.on_resolved = [&loop, &scope, &cancellation_delivered] {
                    // Inbox callbacks run before the next ready batch: the source
                    // returns successfully, then cancellation precedes its parent.
                    loop.post([&scope, &cancellation_delivered] {
                        cancellation_delivered = true;
                        scope.cancel();
                    });
                };
                co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
                ip_source.on_resolved = {};
            });
        });

        EXPECT_TRUE(cancellation_delivered);
        EXPECT_EQ(ip_source.calls, 1);
        EXPECT_EQ(resolver.calls, 0);
        EXPECT_EQ(gateway.calls, 0);
    }
}

TEST(RunRoot, UpdateBudgetTimeoutIsAFailedCycleNotAShutdown) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.hang = 3600s;  // every cycle's driver call parks past UPDATE_BUDGET
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);

    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(75s, [&]() -> coro::Task<void> {
            co_await app::subdomain_loop(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0, t=30 and t=60 each burn the full 30s UPDATE_BUDGET and
    // fail; the loop must keep scheduling after a budget timeout instead of
    // retiring the subdomain (the timer marks the scope cancelled, and the
    // loop used to read that as a shutdown). A burnt budget exceeds the 10s
    // interval, so the next cycle starts immediately (catch-up anchoring).
    // The t=75 outer budget cancels cycle three's parked driver call.
    EXPECT_EQ(gateway.calls, 3);
}

TEST(RunRoot, IpSourceTimeoutSkipsTheCycle) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.hang = 3600s;  // never answers within the source budget
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};
    const auto start = clock.now();

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    // The source's own budget fired: a transient failure that skips this cycle,
    // not an abort and not a cycle-budget fire.
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(resolver.calls, 0);
    EXPECT_EQ(gateway.calls, 0);
    EXPECT_GE(clock.now() - start, std::chrono::seconds(10));
}

// ---------------------------------------------------------------------------
// run_update_cycle decisions
// ---------------------------------------------------------------------------

TEST(RunRoot, RunUpdateCycleSkipsWhenTheRecordIsUnchanged) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::SKIP_UNCHANGED);
    EXPECT_EQ(gateway.calls, 0);
}

TEST(RunRoot, RunUpdateCycleUpdatesWhenTheRecordChanged) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    ASSERT_EQ(gateway.fqdns.size(), 1u);
    EXPECT_EQ(gateway.fqdns[0], "www.example.com");
}

TEST(RunRoot, RunUpdateCycleForceSkipsTheResolver) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", true};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_FORCED);
    EXPECT_EQ(resolver.calls, 0);
    EXPECT_EQ(gateway.calls, 1);
}

TEST(RunRoot, RunUpdateCyclePropagatesDriverRetryAfter) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.fail_first_retry_after = 42;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_EQ(outcome.error().retry_after_seconds, 42);
}

TEST(RunRoot, RunUpdateCycleSkipsWhenNoLocalAddress) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.addresses.clear();
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(gateway.calls, 0);
}

// ---------------------------------------------------------------------------
// DNS read budget (timeout is a scope property composed in the workflow)
// ---------------------------------------------------------------------------

TEST(RunRoot, RunUpdateCycleDnsReadTimeoutStillPublishes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.hang = 3600s;  // never answers within the read budget
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};
    const auto start = clock.now();

    std::expected<app::UpdateCycleResult, domain::UpdateError> outcome;
    run_loop(loop, [&]() -> coro::Task<void> {
        outcome = co_await app::run_update_cycle(task, services);
        co_return;
    });

    // The read's own budget fired: it is a transient failure, so the update
    // still goes out (empty record list decides UPDATE_CHANGED).
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    EXPECT_GE(clock.now() - start, std::chrono::seconds(4));
}

TEST(RunRoot, RunUpdateCycleExternalCancelDoesNotPublish) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.hang = 3600s;
    FakeIpSource ip_source;
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 0);
    const domain::UpdateTask task{config->domains[0], config->domains[0].subdomains[0], "www.example.com", false};

    bool returned = false;
    bool cancelled = false;
    run_loop(loop, [&]() -> coro::Task<void> {
        // The outer scope fires at 2s, before the 4s read budget: an external
        // cancel, not our timeout, must abort without publishing.
        const auto outcome = co_await coro::with_timeout(2s, [&]() -> coro::Task<void> {
            co_await app::run_update_cycle(task, services);
            returned = true;
            co_return;
        });
        cancelled = outcome.timed_out && !outcome.completed;
        co_return;
    });

    EXPECT_FALSE(returned);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(gateway.calls, 0);
}

// ---------------------------------------------------------------------------
// next_delay
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// run_root: supervisor semantics, SIGINT shutdown and the exit status
// ---------------------------------------------------------------------------

TEST(RunRoot, RunRootSupervisesSubdomainsAndStopsOnSigint) {
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.push_back("bad");
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    // The second successful cycle asks the process to shut down, which proves
    // the healthy subdomain kept running after its sibling died.
    ProbeGateway::State* state = gateway_state.get();
    gateway_state->on_call = [state] {
        if (state->calls >= 2) {
            ::kill(::getpid(), SIGINT);
        }
    };
    NullLogger logger;

    const auto config = make_config({make_subdomain("bad", 1), make_subdomain("good", 1)}, 1, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .drain_logs = nullptr,
    };

    const int code = coro::run(loop, app::run_root(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    // The healthy subdomain updated at least twice; the failing one never did.
    EXPECT_GE(gateway_state->calls, 2);
    for (const auto& fqdn : gateway_state->fqdns) {
        EXPECT_EQ(fqdn, "good.example.com");
    }
    EXPECT_FALSE(resolver.calls == 0);
}

TEST(RunRoot, RunRootReturnsFailureWhenEveryLoopDies) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.push_back("only");
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    NullLogger logger;

    const auto config = make_config({make_subdomain("only", 1)}, 1, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .drain_logs = nullptr,
    };

    const int code = coro::run(loop, app::run_root(config, runtime_services));

    // No shutdown was requested, but every subdomain loop died of a defect:
    // the process is functionally dead and must report a failure status so a
    // supervisor (Restart=on-failure) starts it again.
    EXPECT_EQ(code, EXIT_FAILURE);
    EXPECT_EQ(gateway_state->calls, 0);
}

TEST(RunRoot, SubdomainDefectIsReportedAndSiblingsSurvive) {
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.push_back("bad");
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    // The second successful cycle asks the process to shut down, which proves
    // the healthy subdomain kept running after its sibling died.
    ProbeGateway::State* state = gateway_state.get();
    gateway_state->on_call = [state] {
        if (state->calls >= 2) {
            ::kill(::getpid(), SIGINT);
        }
    };
    RecordingLogger logger;

    const auto config = make_config({make_subdomain("bad", 1), make_subdomain("good", 1)}, 1, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .drain_logs = nullptr,
    };

    const int code = coro::run(loop, app::run_root(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_GE(gateway_state->calls, 2);
    // A dead loop must never vanish silently: the defect is reported once,
    // naming the subdomain.
    bool reported = false;
    for (const auto& record : logger.records()) {
        const auto level = record.level;
        const auto& message = record.message;
        if (level == app::LogLevel::ERROR && message.find("bad.example.com") != std::string::npos &&
            message.find("died") != std::string::npos) {
            reported = true;
        }
    }
    EXPECT_TRUE(reported);
}

TEST(RunRoot, RunRoot_AncestorCancellation_DrainsAndPropagates) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    NullLogger logger;
    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .drain_logs = nullptr,
    };
    bool root_returned = false;
    auto task = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(5s, [&]() -> coro::Task<void> {
            co_await app::run_root(config, services);
            root_returned = true;
        });
        EXPECT_TRUE(outcome.timed_out);
        EXPECT_FALSE(outcome.completed);
    };
    coro::run(loop, task());
    EXPECT_FALSE(root_returned);
    EXPECT_EQ(gateway_state->calls, 1);
}
