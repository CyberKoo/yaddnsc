//
// Main-loop tests — the per-subdomain task coroutine and the main loop.
//
// Timeline behaviour is deterministic: the loop runs on a ManualClock, so
// "sleep for the update interval" really advances simulated time. The run-root
// supervisor/SIGINT test uses the system clock with a 1s interval.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; coroutine bodies use EXPECT_* only.
//

#include "application/tasks/main_loop.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <coroutine>

#include <expected>
#include <gtest/gtest.h>
#include <unistd.h>

#include "application/ports/gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/log.h"
#include "application/ports/resolver.h"
#include "application/services.h"
#include "application/tasks/subdomain_task.h"
#include "application/tasks/update_task.h"
#include "coro/coro.h"
#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
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

    /// Validation surface: counts calls, optionally rejects or hangs.
    struct Validation {
        bool available{true};
        int calls{0};
        std::optional<domain::DriverError> rejection;
        std::chrono::seconds hang{0};
    } validation{};

    [[nodiscard]] bool supports_validate(std::string_view) const override { return validation.available; }

    coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string, std::string) override {
        ++validation.calls;
        if (validation.hang.count() > 0) {
            co_await coro::sleep_for(validation.hang);
        }
        if (validation.rejection.has_value()) {
            co_return std::unexpected(*validation.rejection);
        }
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

/// A gateway whose bookkeeping lives in shared state. The main loop owns the
/// gateway (unique_ptr factory) and destroys it when the run ends, so a test
/// that drives main_loop inspects this shared state instead of the object.
class ProbeGateway final : public app::GatewayPort {
public:
    struct State {
        int calls = 0;
        bool destroyed = false;
        bool child_drained = false;
        std::vector<std::string> fqdns;

        /// Startup-validation surface, shared like the rest of the probe state.
        struct Validation {
            bool available{true};
            int calls{0};
            std::optional<domain::DriverError> rejection;
            std::chrono::seconds hang{0};
        } validation{};

        /// When positive, update() parks this long inside a shielded scope — a
        /// driver that keeps working through the shutdown request, which keeps
        /// the drain phase (and the signal watchers) alive for its duration.
        std::chrono::seconds shielded_hang{0};
        /// Invoked on every call, after the counters are updated.
        std::function<void()> on_call;
    };

    explicit ProbeGateway(std::shared_ptr<State> state) : state_(std::move(state)) {}

    ~ProbeGateway() noexcept override { state_->destroyed = true; }

    [[nodiscard]] bool supports_validate(std::string_view) const override { return state_->validation.available; }

    coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string, std::string) override {
        ++state_->validation.calls;
        if (state_->validation.hang.count() > 0) {
            co_await coro::sleep_for(state_->validation.hang);
        }
        if (state_->validation.rejection.has_value()) {
            co_return std::unexpected(*state_->validation.rejection);
        }
        co_return std::expected<void, domain::DriverError>{};
    }

    coro::Task<std::expected<void, domain::DriverError>> update(std::string,
                                                                domain::DriverUpdateCommand command) override {
        ++state_->calls;
        state_->fqdns.push_back(command.fqdn);
        if (state_->on_call) {
            state_->on_call();
        }
        if (state_->shielded_hang.count() > 0) {
            const auto hang = state_->shielded_hang;
            co_await coro::non_cancellable([hang](coro::CancelScope&) -> coro::Task<void> {
                co_await coro::sleep_for(hang);
                co_return;
            });
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

/// Drive the main loop for tests that assert on how it fails rather than on the
/// status it returns. coro::run is [[nodiscard]]; the failure is the outcome.
void run_main_loop(coro::Loop& loop, const std::shared_ptr<const domain::RuntimeConfig>& config,
                   const app::RuntimeServices& services) {
    [[maybe_unused]] const int code = coro::run(loop, app::main_loop(config, services));
}

}  // namespace

// ---------------------------------------------------------------------------
// subdomain_task timing (ManualClock)
// ---------------------------------------------------------------------------

TEST(MainLoop, SubdomainTaskRunsOnTheUpdateInterval) {
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
            co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0, t=10, t=20; the t=25 budget cancels the sleep towards t=30.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 3);
}

TEST(MainLoop, RetryAfterOverridesTheUpdateInterval) {
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
            co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // t=0 fails with retry_after=30, so the next attempt is at t=30, then t=40.
    EXPECT_EQ(gateway.calls, 3);
}

TEST(MainLoop, ForceUpdateIntervalSkipsTheDnsRead) {
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
            co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // Cycles at t=0 (forced), t=10, t=20 (forced at 20 >= 15): two forced
    // cycles skip the DNS read, so only the t=10 cycle consults the resolver.
    EXPECT_EQ(gateway.calls, 3);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(MainLoop, CancellationWakesTheLoopFromItsSleep) {
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
            co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
            co_return;
        });
        co_return;
    });

    // One cycle at t=0; the t=5 budget cancels the 100s sleep, so the loop exits
    // without a second cycle.
    EXPECT_EQ(gateway.calls, 1);
}

TEST(MainLoop, SubdomainTask_AlreadyCancelled_DoesNotResolveDnsOrPublish) {
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
                co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
            });
        });

        // Awaiting a Task starts its body even in a cancelled scope. The source
        // result's scope outcome stops the cycle; no later port is entered.
        EXPECT_EQ(ip_source.calls, 1);
        EXPECT_EQ(resolver.calls, 0);
        EXPECT_EQ(gateway.calls, 0);
    }
}

TEST(MainLoop, SubdomainTask_SourceCompletedBeforeCancel_DoesNotResolveDnsOrPublish) {
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
                co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
                ip_source.on_resolved = {};
            });
        });

        EXPECT_TRUE(cancellation_delivered);
        EXPECT_EQ(ip_source.calls, 1);
        EXPECT_EQ(resolver.calls, 0);
        EXPECT_EQ(gateway.calls, 0);
    }
}

TEST(MainLoop, UpdateBudgetTimeoutIsAFailedCycleNotAShutdown) {
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
            co_await app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services);
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

TEST(MainLoop, IpSourceTimeoutSkipsTheCycle) {
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
        outcome = co_await app::update_task(task, services);
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
// update_task decisions
// ---------------------------------------------------------------------------

TEST(MainLoop, UpdateTaskSkipsWhenTheRecordIsUnchanged) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::SKIP_UNCHANGED);
    EXPECT_EQ(gateway.calls, 0);
}

TEST(MainLoop, UpdateTaskUpdatesWhenTheRecordChanged) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    ASSERT_EQ(gateway.fqdns.size(), 1u);
    EXPECT_EQ(gateway.fqdns[0], "www.example.com");
}

TEST(MainLoop, UpdateTaskForceSkipsTheResolver) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_FORCED);
    EXPECT_EQ(resolver.calls, 0);
    EXPECT_EQ(gateway.calls, 1);
}

TEST(MainLoop, UpdateTaskPropagatesDriverRetryAfter) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_EQ(outcome.error().retry_after_seconds, 42);
}

TEST(MainLoop, UpdateTaskSkipsWhenNoLocalAddress) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(gateway.calls, 0);
}

// ---------------------------------------------------------------------------
// DNS read budget (timeout is a scope property composed in the workflow)
// ---------------------------------------------------------------------------

TEST(MainLoop, UpdateTaskDnsReadTimeoutStillPublishes) {
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
        outcome = co_await app::update_task(task, services);
        co_return;
    });

    // The read's own budget fired: it is a transient failure, so the update
    // still goes out (empty record list decides UPDATE_CHANGED).
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(gateway.calls, 1);
    EXPECT_GE(clock.now() - start, std::chrono::seconds(4));
}

TEST(MainLoop, UpdateTaskExternalCancelDoesNotPublish) {
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
            co_await app::update_task(task, services);
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
// main_loop: supervisor semantics, SIGINT shutdown and the exit status
// ---------------------------------------------------------------------------

TEST(MainLoop, MainLoopSupervisesSubdomainsAndStopsOnSigint) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.emplace_back("bad");
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
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    // The healthy subdomain updated at least twice; the failing one never did.
    EXPECT_GE(gateway_state->calls, 2);
    for (const auto& fqdn : gateway_state->fqdns) {
        EXPECT_EQ(fqdn, "good.example.com");
    }
    EXPECT_FALSE(resolver.calls == 0);
}

TEST(MainLoop, SubdomainDefectRetriesTheCycleAfterTheInterval) {
    const coro::StopSignals stop;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.emplace_back("only");
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    RecordingLogger logger;

    const auto config = make_config({make_subdomain("only", 10)}, 10, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    auto task = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(
            35s, [&]() -> coro::Task<void> { co_await app::main_loop(config, runtime_services); });
        // A defect no longer retires the subdomain: the daemon keeps running
        // until something cancels it (here: the test's budget).
        EXPECT_TRUE(outcome.timed_out);
    };
    coro::run(loop, task());

    // Defects at t=0, 10, 20, 30: the legacy queue retried a failed task on
    // the interval, and so does the subdomain task.
    EXPECT_EQ(ip_source.calls, 4);
    EXPECT_EQ(gateway_state->calls, 0);

    // A failed cycle never vanishes silently: every defect is reported, naming
    // the subdomain.
    int failures = 0;
    for (const auto& record : logger.records()) {
        if (record.level == app::LogLevel::ERROR && record.message.find("only.example.com") != std::string::npos &&
            record.message.find("failed") != std::string::npos) {
            ++failures;
        }
    }
    EXPECT_GE(failures, 3);
}

TEST(MainLoop, SubdomainDefectIsReportedAndSiblingsSurvive) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    ip_source.fatal_subdomains.emplace_back("bad");
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
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_GE(gateway_state->calls, 2);
    // A failed cycle must never vanish silently: the defect is reported,
    // naming the subdomain.
    bool reported = false;
    for (const auto& record : logger.records()) {
        const auto level = record.level;
        const auto& message = record.message;
        if (level == app::LogLevel::ERROR && message.find("bad.example.com") != std::string::npos &&
            message.find("failed") != std::string::npos) {
            reported = true;
        }
    }
    EXPECT_TRUE(reported);
}

TEST(MainLoop, SigtermStopsTheRunWithSuccess) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    ProbeGateway::State* state = gateway_state.get();
    gateway_state->on_call = [state] {
        if (state->calls >= 1) {
            ::kill(::getpid(), SIGTERM);
        }
    };
    NullLogger logger;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_EQ(gateway_state->calls, 1);
}

TEST(MainLoop, AlreadyRequestedStopSkipsValidationAndUpdates) {
    const coro::StopSignals stop;
    ::kill(::getpid(), SIGINT);
    // An earlier request is latched; no watcher scheduling order is involved.
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    NullLogger logger;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_EQ(gateway_state->calls, 0);
    EXPECT_EQ(gateway_state->validation.calls, 0);
}

/// The death-test scenario, factored out: EXPECT_EXIT is a macro and the
/// designated initializers below contain top-level commas it would split on.
void run_until_second_sigint() {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    // The shielded hang keeps the drain phase alive long enough for the
    // second SIGINT to reach the same process handler during drain.
    gateway_state->shielded_hang = 1s;
    gateway_state->on_call = [] {
        std::thread([] {
            ::kill(::getpid(), SIGINT);
            std::this_thread::sleep_for(50ms);
            ::kill(::getpid(), SIGINT);
        }).detach();
    };
    NullLogger logger;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };
    const int code = coro::run(loop, app::main_loop(config, runtime_services));
    ::_exit(code);  // reached only when the escalation did not fire
}

TEST(MainLoop, SecondSigintForcesImmediateTermination) {
    EXPECT_EXIT(run_until_second_sigint(), ::testing::ExitedWithCode(128 + SIGINT), "");
}

// ---------------------------------------------------------------------------
// main_loop: startup driver-parameter validation
// ---------------------------------------------------------------------------

TEST(MainLoop, StartupValidation_RejectionFailsBeforeAnyUpdate) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    NullLogger logger;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    gateway_state->validation.rejection =
        domain::DriverError{domain::DriverError::Code::UPDATE_FAILED, "missing url", 0};

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    // A driver that says its parameters are wrong must not have any update
    // published against them, and the startup has to fail so a supervisor does
    // not restart into the same mistake.
    EXPECT_EQ(code, EXIT_FAILURE);
    EXPECT_EQ(gateway_state->validation.calls, 1);
    EXPECT_EQ(gateway_state->calls, 0);
}

TEST(MainLoop, StartupValidation_DriverWithoutEntryWarnsAndRuns) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    resolver.records = {"198.51.100.1"};
    FakeIpSource ip_source;
    RecordingLogger logger;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    gateway_state->validation.available = false;
    ProbeGateway::State* state = gateway_state.get();
    gateway_state->on_call = [state] {
        if (state->calls >= 1) {
            ::kill(::getpid(), SIGINT);
        }
    };

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    // A plugin built against an older ABI cannot stop a daemon that updates
    // correctly; the run warns and proceeds.
    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_EQ(gateway_state->validation.calls, 0);
    EXPECT_EQ(gateway_state->calls, 1);

    bool warned = false;
    for (const auto& record : logger.records()) {
        if (record.level == app::LogLevel::WARN && record.message.find("fake-driver") != std::string::npos &&
            record.message.find("no configuration validation") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(MainLoop, StartupValidation_TimeoutFailsBeforeAnyUpdate) {
    const coro::StopSignals stop;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    NullLogger logger;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    gateway_state->validation.hang = 3600s;  // never answers within the budget

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
        .policy = app::UpdatePolicy{.driver_validation_budget = 5s},
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_FAILURE);
    EXPECT_EQ(gateway_state->calls, 0);
}

TEST(MainLoop, StartupValidation_AncestorCancellationSkipsUpdates) {
    const coro::StopSignals stop;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    NullLogger logger;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    gateway_state->validation.hang = 3600s;  // parks inside the budget
    // The validation hangs, so the run is cancelled from outside — the shape a
    // SIGINT during a slow startup has.
    bool completed = false;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
        .policy = app::UpdatePolicy{.driver_validation_budget = 3600s},
    };

    const auto outcome = coro::run(loop, coro::with_timeout(5s, [&]() -> coro::Task<int> {
                                       co_return co_await app::main_loop(config, runtime_services);
                                   }));
    completed = !outcome.timed_out;

    // Validation runs inside the cancellable scope, so the budget is a shield
    // the outer scope tears down rather than something the run has to finish.
    EXPECT_FALSE(completed);
    EXPECT_EQ(gateway_state->calls, 0);
    EXPECT_EQ(gateway_state->validation.calls, 1);
}

TEST(MainLoop, StopSignalCancelsStartupValidation) {
    const coro::StopSignals stop;
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    NullLogger logger;
    const auto state = std::make_shared<ProbeGateway::State>();
    state->validation.hang = 3600s;
    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
        .policy = app::UpdatePolicy{.driver_validation_budget = 3600s},
    };
    std::thread sender([] {
        std::this_thread::sleep_for(50ms);
        ::kill(::getpid(), SIGTERM);
    });
    const int code = coro::run(loop, app::main_loop(config, services));
    sender.join();
    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_EQ(state->validation.calls, 1);
    EXPECT_EQ(state->calls, 0);
    EXPECT_TRUE(state->destroyed);
}

TEST(MainLoop, SigtermDuringGatewayConstructionStopsBeforeAnyUpdate) {
    const coro::StopSignals stop;
    // The same handler latches a stop during gateway construction. No watcher
    // has to be armed yet, and neither validation nor updates may start.

    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    NullLogger logger;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            ::kill(::getpid(), SIGTERM);
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };

    const int code = coro::run(loop, app::main_loop(config, runtime_services));

    EXPECT_EQ(code, EXIT_SUCCESS);
    EXPECT_TRUE(stop.requested());
    EXPECT_EQ(gateway_state->calls, 0);
    EXPECT_TRUE(gateway_state->destroyed);
}

TEST(MainLoop, StopWaitFailureCancelsRunBeforeAnyUpdate) {
    const coro::StopSignals stop;
    // A defect inside the awaited task must reach the fail-fast root group,
    // rather than being hidden in a side channel until a handover checkpoint.
    coro::Loop loop;
    FakeResolver resolver;
    FakeIpSource ip_source;
    const auto gateway_state = std::make_shared<ProbeGateway::State>();
    NullLogger logger;

    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices runtime_services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [&gateway_state](coro::TaskGroup&) -> std::unique_ptr<app::GatewayPort> {
            return std::make_unique<ProbeGateway>(gateway_state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = []() -> coro::Task<void> {
            throw std::invalid_argument("stop wait failed");
            co_return;
        },
    };

    EXPECT_THROW(run_main_loop(loop, config, runtime_services), std::invalid_argument);
    EXPECT_EQ(gateway_state->calls, 0);
}

coro::Task<void> gateway_child_cleanup(std::shared_ptr<ProbeGateway::State> state) {
    bool cancelled = false;
    try {
        co_await coro::sleep_for(100s);
    } catch (const coro::Cancelled&) {
        cancelled = true;
    }
    EXPECT_TRUE(cancelled);
    co_await coro::non_cancellable([state](coro::CancelScope&) -> coro::Task<void> {
        co_await coro::sleep_for(10ms);
        EXPECT_FALSE(state->destroyed);
        state->child_drained = true;
    });
}

TEST(MainLoop, AncestorCancellation_KeepsGatewayAliveThroughBridgeChildCleanup) {
    const coro::StopSignals stop;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    FakeIpSource ip_source;
    NullLogger logger;
    const auto state = std::make_shared<ProbeGateway::State>();
    const auto config = make_config({make_subdomain("www", 100)}, 100, 0);
    const app::RuntimeServices services{
        .resolver = resolver,
        .ip_source = ip_source,
        .logger = logger,
        .make_gateway = [state](coro::TaskGroup& root) -> std::unique_ptr<app::GatewayPort> {
            root.spawn_discard(gateway_child_cleanup(state));
            return std::make_unique<ProbeGateway>(state);
        },
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };
    auto task = [&]() -> coro::Task<void> {
        auto outcome = co_await coro::with_timeout(5s, [&]() { return app::main_loop(config, services); });
        EXPECT_TRUE(outcome.timed_out);
        EXPECT_TRUE(state->child_drained);
        EXPECT_TRUE(state->destroyed);
    };
    coro::run(loop, task());
    EXPECT_EQ(clock.now().time_since_epoch(), 5010ms);
}

TEST(MainLoop, MainLoop_AncestorCancellation_DrainsAndPropagates) {
    const coro::StopSignals stop;
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
        .stop_requested = [&stop] { return stop.requested(); },
        .wait_for_stop = [&stop] { return stop.wait(); },
    };
    bool root_returned = false;
    auto task = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(5s, [&]() -> coro::Task<void> {
            co_await app::main_loop(config, services);
            root_returned = true;
        });
        EXPECT_TRUE(outcome.timed_out);
        EXPECT_FALSE(outcome.completed);
    };
    coro::run(loop, task());
    EXPECT_FALSE(root_returned);
    EXPECT_EQ(gateway_state->calls, 1);
}

TEST(MainLoop, SubdomainTask_DefectAfterForcedPublish_PreservesTheRestartForcePolicy) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    ip_source.on_resolved = [&] {
        if (ip_source.calls == 2) {
            throw std::bad_alloc{};
        }
    };
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 60);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(
            35s, [&]() { return app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services); });
    });
    // Publish at t=0; defect at t=10; force again at t=20; DNS check at t=30.
    EXPECT_EQ(ip_source.calls, 4);
    EXPECT_EQ(gateway.calls, 2);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(MainLoop, SubdomainTask_FailedForcedPublish_RetriesBeforeNextForceInterval) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.fail_first_retry_after = 10;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 60);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(
            25s, [&]() { return app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services); });
    });
    EXPECT_EQ(gateway.calls, 2);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(MainLoop, SubdomainTask_ForcedPublishTimesOut_RemainsDue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    FakeGateway gateway;
    gateway.on_call = [&] { gateway.hang = gateway.calls == 1 ? 3600s : 0s; };
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 60);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(
            45s, [&]() { return app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services); });
    });
    EXPECT_EQ(gateway.calls, 2);
    EXPECT_EQ(resolver.calls, 1);
}

TEST(MainLoop, SubdomainTask_NoLocalAddress_RemainsDueForForcedPublish) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    FakeResolver resolver;
    resolver.records = {"192.0.2.1"};
    FakeIpSource ip_source;
    ip_source.addresses.clear();
    ip_source.on_resolved = [&] {
        if (ip_source.calls > 1)
            ip_source.addresses = {address("192.0.2.1")};
    };
    FakeGateway gateway;
    NullLogger logger;
    const app::Services services{resolver, ip_source, gateway, logger};
    const auto config = make_config({make_subdomain("www", 10)}, 10, 60);
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::with_timeout(
            25s, [&]() { return app::subdomain_task(config->domains[0], config->domains[0].subdomains[0], services); });
    });
    EXPECT_EQ(gateway.calls, 1);
    EXPECT_EQ(resolver.calls, 1);
}
