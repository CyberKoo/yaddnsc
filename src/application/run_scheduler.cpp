//
// app — the run root (implementation).
//

#include "run_scheduler.h"

#include <csignal>
#include <cstdlib>
#include <cstddef>
#include <memory>
#include <utility>

#include <unistd.h>

#include "application/subdomain_loop.h"
#include "application/ports/log.h"
#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/signal.hpp"
#include "support/fmt.hpp"

namespace app {

namespace {

/// Watch one signal and cancel the work scope when it arrives.
///
/// SIGINT escalation matches the legacy SignalWatcher: the first SIGINT asks for
/// a graceful shutdown, the second terminates immediately with the conventional
/// 128 + SIGINT status. SIGTERM only ever asks for shutdown.
///
/// Cancellation: when the root scope is cancelled the parked on_signal await
/// returns `operation_canceled`, which ends this watcher.
coro::Task<void> watch_signal(int sig, bool escalate, int& sigint_count, coro::CancelScope*& work_scope,
                              const Logger& logger, const std::function<void()>& drain_logs) {
    for (;;) {
        const auto delivered = co_await coro::on_signal(sig);
        if (!delivered.has_value()) {
            // The root scope was cancelled: the run is tearing down.
            co_return;
        }
        if (escalate) {
            if (++sigint_count >= 2) {
                YLOG_CRITICAL(logger, "Second SIGINT received, forcing immediate termination");
                // _exit skips the normal drain in main(): empty the async queue
                // now, or the critical line above (and anything still queued)
                // would be lost — the legacy synchronous logger wrote before
                // exiting.
                if (drain_logs) {
                    drain_logs();
                }
                ::_exit(128 + sig);
            }
            YLOG_INFO(logger, "Received SIGINT, initiating graceful shutdown (press Ctrl-C again to force quit)...");
        } else {
            YLOG_INFO(logger, "Received SIGTERM, shutting down...");
        }
        if (work_scope != nullptr) {
            work_scope->cancel();
        }
    }
}

}  // namespace

namespace {

/// Drive one subdomain loop and report a defect that escapes it. The
/// supervisor group keeps the siblings alive, but a dead loop must never
/// vanish silently.
coro::Task<void> guarded_subdomain_loop(const std::shared_ptr<const domain::RuntimeConfig>& config,
                                        std::size_t domain_index, std::size_t subdomain_index,
                                        const Services& services) {
    const std::string fqdn =
        config->domains[domain_index].subdomains[subdomain_index].name + '.' + config->domains[domain_index].name;
    try {
        co_await subdomain_loop(config, domain_index, subdomain_index, services);
    } catch (const std::exception& error) {
        YLOG_ERROR(services.logger, "Subdomain loop for {} died: {}", fqdn, error.what());
    } catch (...) {
        YLOG_ERROR(services.logger, "Subdomain loop for {} died with a non-standard exception", fqdn);
    }
}

}  // namespace

coro::Task<int> run_scheduler(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    std::size_t task_count = 0;
    for (const auto& domain : config->domains) {
        task_count += domain.subdomains.size();
    }
    YLOG_INFO(services.logger, "Scheduler initialised with {} tasks", task_count);

    bool shutdown_requested = false;
    co_await coro::supervisor_group([&](coro::TaskGroup& root) -> coro::Task<void> {
        GatewayPort& gateway = services.make_gateway(root);
        const Services svc{services.resolver, services.ip_source, gateway, services.logger};

        // Watchers live in the root scope, not the work scope, so the shutdown
        // they trigger does not end them; the second SIGINT can still be caught.
        // They are cancelled explicitly by root.cancel() once the work is done.
        int sigint_count = 0;
        coro::CancelScope* work_scope = nullptr;
        root.spawn(watch_signal(SIGINT, true, sigint_count, work_scope, services.logger, services.drain_logs));
        root.spawn(watch_signal(SIGTERM, false, sigint_count, work_scope, services.logger, services.drain_logs));

        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            work_scope = &scope;
            co_await coro::supervisor_group([&](coro::TaskGroup& work) -> coro::Task<void> {
                const auto& cfg = *config;
                for (std::size_t di = 0; di < cfg.domains.size(); ++di) {
                    for (std::size_t si = 0; si < cfg.domains[di].subdomains.size(); ++si) {
                        work.spawn(guarded_subdomain_loop(config, di, si, svc));
                    }
                }
                co_return;
            });
            // A subdomain loop only returns under cancellation, so reaching
            // here with a live scope means every loop died of a defect.
            shutdown_requested = scope.cancelled();
        });
        work_scope = nullptr;

        // The work is finished: stop the watchers so the root group can join.
        root.cancel();
        co_return;
    });
    YLOG_INFO(services.logger, "All tasks drained, shutting down");
    if (task_count > 0 && !shutdown_requested) {
        // Every loop is dead and no shutdown was asked for: the daemon is
        // functionally dead. Exit with a failure status so a supervisor
        // (Restart=on-failure) starts it again instead of leaving a husk.
        YLOG_CRITICAL(services.logger, "All subdomain loops have died; exiting with a failure status");
        co_return EXIT_FAILURE;
    }
    co_return EXIT_SUCCESS;
}

}  // namespace app
