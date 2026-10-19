//
// app — the main loop (implementation).
//

#include "main_loop.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <string>
#include <utility>
#include <vector>

#include "application/config_validation.h"
#include "application/log_macros.h"
#include "application/ports/gateway.h"
#include "application/services.h"
#include "application/tasks/subdomain_task.h"
#include "coro/cancel_scope.h"
#include "coro/fwd.h"
#include "coro/nursery.hpp"
#include "coro/with_nursery.hpp"
#include "domain/config/runtime_config.h"

namespace app {

namespace {

/// The process adapter latches the request and handles repeated Ctrl-C itself.
coro::Task<void> watch_stop(const RuntimeServices& services, coro::CancelScope*& work_scope) {
    co_await services.wait_for_stop();
    YLOG_INFO(services.logger, "Received stop signal, shutting down...");
    if (work_scope != nullptr) {
        work_scope->cancel();
    }
}

/// Number of configured subdomain records — the unit the work phase announces
/// and the one the exit status reasons about.
[[nodiscard]] std::size_t count_records(const domain::RuntimeConfig& config) noexcept {
    std::size_t total = 0;
    for (const auto& domain : config.domains) {
        total += domain.subdomains.size();
    }
    return total;
}

/// Validate before publishing, then supervise one task per subdomain.
/// Publish the group's scope so the stop watcher can cancel validation/work.
/// Returns false on validation rejection; cancellation follows scope semantics.
coro::Task<bool> run_work(const std::shared_ptr<const domain::RuntimeConfig>& config, const Services& services,
                          coro::CancelScope*& work_scope, const std::function<bool()>& stop_requested) {
    // Runner-owned publication: only its post-drain destruction clears the borrow.
    struct WorkResources {
        coro::CancelScope** published;

        explicit WorkResources(coro::CancelScope*& scope) noexcept : published(&scope) {}

        WorkResources(WorkResources&& other) noexcept : published(std::exchange(other.published, nullptr)) {}

        WorkResources(const WorkResources&) = delete;
        WorkResources& operator=(const WorkResources&) = delete;
        WorkResources& operator=(WorkResources&&) = delete;

        ~WorkResources() noexcept {
            if (published != nullptr) {
                *published = nullptr;
            }
        }
    };

    bool rejected = false;
    co_await coro::with_nursery(
        WorkResources{work_scope},
        [&](coro::Nursery& work, WorkResources&) -> coro::Task<void> {
            work_scope = &work.scope();
            if (stop_requested()) {
                work.cancel();
                co_return;
            }
            work.scope().throw_if_cancelled();

            // Missing optional validation is compatible; explicit rejection
            // stops startup before any update task is spawned.
            const auto validated =
                co_await validate_driver_configs(services.gateway, *config, ConfigValidationMode::WARN_AND_CONTINUE,
                                                 services.policy.driver_validation_budget);
            for (const auto& driver : validated.unchecked_drivers) {
                YLOG_WARN(services.logger,
                          "Driver '{}' provides no configuration validation; its driver_params were not checked",
                          driver);
            }
            if (!validated.rejection.empty()) {
                YLOG_CRITICAL(services.logger, "{}", validated.rejection);
                rejected = true;
                work.cancel();
                co_return;
            }

            if (stop_requested()) {
                work.cancel();
                co_return;
            }
            YLOG_INFO(services.logger, "Starting update loops for {} records", count_records(*config));

            for (const auto& domain : config->domains) {
                for (const auto& subdomain : domain.subdomains) {
                    work.spawn(subdomain_task(domain, subdomain, services));
                }
            }
        },
        coro::NurseryPolicy::SUPERVISOR);
    co_return !rejected;
}

}  // namespace

coro::Task<MainLoopOutcome> main_loop(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    const std::size_t task_count = count_records(*config);

    coro::CancelScope* work_scope = nullptr;
    // The work phase reports whether it got as far as running the loops: a
    // rejected configuration leaves nothing running and is a startup failure.
    bool ran_updates = false;

    struct RootResources {
        std::unique_ptr<GatewayPort> gateway;
        std::optional<Services> services;
    };

    co_await coro::with_nursery(
        RootResources{}, [&](coro::Nursery& root, RootResources& resources) -> coro::Task<void> {
            resources.gateway = services.make_gateway(root);
            resources.services.emplace(services.resolver, services.ip_source, *resources.gateway, services.logger,
                                       services.policy);
            root.spawn(watch_stop(services, work_scope));
            ran_updates = co_await run_work(config, *resources.services, work_scope, services.stop_requested);
            root.cancel();
        });
    YLOG_INFO(services.logger, "All tasks drained, shutting down");

    if (!ran_updates && !services.stop_requested()) {
        // run_work returns false only when validation rejected the
        // configuration; the rejection itself is already logged. The startup
        // has to fail so a supervisor does not restart into the same mistake.
        co_return MainLoopOutcome::ConfigurationRejected;
    }
    if (task_count > 0 && !services.stop_requested()) {
        // Defensive: subdomain tasks retry failed cycles after defects, so a
        // normal return here means every task retired without a shutdown
        // request — which no current path does. If that ever changes, exit
        // with a failure status so a supervisor (Restart=on-failure) starts
        // the daemon again instead of leaving a husk.
        YLOG_CRITICAL(services.logger, "All subdomain tasks have died; exiting with a failure status");
        co_return MainLoopOutcome::UpdateTasksEnded;
    }
    co_return MainLoopOutcome::GracefulShutdown;
}

}  // namespace app
