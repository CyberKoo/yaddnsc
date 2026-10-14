//
// app — the main loop (implementation).
//

#include "main_loop.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <string>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include "application/diagnostics.h"
#include "application/log_macros.h"
#include "application/services.h"
#include "application/tasks/subdomain_task.h"
#include "coro/cancel_scope.h"
#include "coro/group.hpp"
#include "coro/task_group.hpp"
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
    // The group may propagate a body defect or ancestor cancellation. Clear
    // the borrowed scope on every body exit while the scope is still alive.
    struct ResetWorkScope {
        coro::CancelScope*& scope;

        ~ResetWorkScope() noexcept { scope = nullptr; }
    };

    bool rejected = false;
    co_await coro::supervisor_group([&](coro::TaskGroup& work) -> coro::Task<void> {
        work_scope = &work.scope();
        const ResetWorkScope reset{work_scope};
        // This phase needs a drain, not cancellable result collection. Keep
        // reset outside the child body so even partial-launch defects cancel
        // and join every loop before the published scope borrow is cleared.
        co_await work.with_children([&]() -> coro::Task<void> {
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
        });
        co_return;
    });
    co_return !rejected;
}

}  // namespace

coro::Task<int> main_loop(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    const std::size_t task_count = count_records(*config);

    coro::CancelScope* work_scope = nullptr;
    // The work phase reports whether it got as far as running the loops: a
    // rejected configuration leaves nothing running and is a startup failure.
    bool ran_updates = false;
    co_await coro::task_group([&](coro::TaskGroup& root) -> coro::Task<void> {
        const auto gateway = services.make_gateway(root);
        const Services svc{services.resolver, services.ip_source, *gateway, services.logger, services.policy};

        // gateway/svc stay in this outer body while with_children drains both
        // stop watcher and bridge requests, including defect/cancellation exits.
        co_await root.with_children([&]() -> coro::Task<void> {
            root.spawn(watch_stop(services, work_scope));
            ran_updates = co_await run_work(config, svc, work_scope, services.stop_requested);
            root.cancel();
        });
        co_return;
    });
    YLOG_INFO(services.logger, "All tasks drained, shutting down");

    if (!ran_updates && !services.stop_requested()) {
        // run_work returns false only when validation rejected the
        // configuration; the rejection itself is already logged. The startup
        // has to fail so a supervisor does not restart into the same mistake.
        co_return EXIT_FAILURE;
    }
    if (task_count > 0 && !services.stop_requested()) {
        // Defensive: subdomain tasks retry failed cycles after defects, so a
        // normal return here means every task retired without a shutdown
        // request — which no current path does. If that ever changes, exit
        // with a failure status so a supervisor (Restart=on-failure) starts
        // the daemon again instead of leaving a husk.
        YLOG_CRITICAL(services.logger, "All subdomain tasks have died; exiting with a failure status");
        co_return EXIT_FAILURE;
    }
    co_return EXIT_SUCCESS;
}

}  // namespace app
