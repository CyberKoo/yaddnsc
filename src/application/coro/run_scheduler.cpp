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

#include "application/coro/subdomain_loop.h"
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
                              const Logger& logger) {
    for (;;) {
        const auto delivered = co_await coro::on_signal(sig);
        if (!delivered.has_value()) {
            // The root scope was cancelled: the run is tearing down.
            co_return;
        }
        if (escalate) {
            if (++sigint_count >= 2) {
                YLOG_CRITICAL(logger, "Second SIGINT received, forcing immediate termination");
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

coro::Task<int> run_scheduler(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    co_await coro::supervisor_group([&](coro::TaskGroup& root) -> coro::Task<void> {
        GatewayPort& gateway = services.make_gateway(root);
        const Services svc{services.resolver, services.ip_source, gateway, services.logger};

        // Watchers live in the root scope, not the work scope, so the shutdown
        // they trigger does not end them; the second SIGINT can still be caught.
        // They are cancelled explicitly by root.cancel() once the work is done.
        int sigint_count = 0;
        coro::CancelScope* work_scope = nullptr;
        root.spawn(watch_signal(SIGINT, true, sigint_count, work_scope, services.logger));
        root.spawn(watch_signal(SIGTERM, false, sigint_count, work_scope, services.logger));

        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            work_scope = &scope;
            co_await coro::supervisor_group([&](coro::TaskGroup& work) -> coro::Task<void> {
                const auto& cfg = *config;
                for (std::size_t di = 0; di < cfg.domains.size(); ++di) {
                    for (std::size_t si = 0; si < cfg.domains[di].subdomains.size(); ++si) {
                        work.spawn(subdomain_loop(config, di, si, svc));
                    }
                }
                co_return;
            });
        });
        work_scope = nullptr;

        // The work is finished (shutdown or a defect in every loop): stop the
        // watchers so the root group can join.
        root.cancel();
        co_return;
    });
    co_return EXIT_SUCCESS;
}

}  // namespace app
