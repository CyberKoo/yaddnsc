//
// app — the run root (implementation).
//

#include "run_root.h"

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <unistd.h>

#include "application/ports/log.h"
#include "application/subdomain_loop.h"
#include "domain/fqdn.h"
#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/signal.hpp"
#include "support/fmt.hpp"

namespace app {

namespace {

/// Shutdown bookkeeping shared between the signal watchers and the work
/// phase: how many SIGINTs have arrived, whether a watcher has asked for
/// shutdown (the exit status reads this bookkeeping, never scope state), and
/// where the cancellable work scope lives (null before the work phase starts
/// and after it ends).
struct ShutdownState {
    int sigint_count = 0;
    bool requested = false;
    coro::CancelScope* work_scope = nullptr;
};

/// Watch one signal and ask for shutdown when it arrives.
///
/// SIGINT escalation matches the legacy SignalWatcher: the first SIGINT asks for
/// a graceful shutdown, the second terminates immediately with the conventional
/// 128 + SIGINT status. SIGTERM only ever asks for shutdown.
///
/// Cancellation: when the root scope is cancelled the parked on_signal await
/// throws `coro::Cancelled`, which unwinds this watcher.
coro::Task<void> watch_signal(int sig, bool escalate, ShutdownState& state, const LoggerPort& logger,
                              const std::function<void()>& drain_logs) {
    for (;;) {
        co_await coro::on_signal(sig);
        if (escalate) {
            if (++state.sigint_count >= 2) {
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
        state.requested = true;
        if (state.work_scope != nullptr) {
            state.work_scope->cancel();
        }
    }
}

/// Drive one subdomain loop and report a defect that escapes it. The
/// supervisor group keeps the siblings alive, but a dead loop must never
/// vanish silently.
coro::Task<void> guarded_subdomain_loop(const domain::DomainConfig& domain, const domain::SubdomainConfig& subdomain,
                                        const Services& services) {
    const std::string fqdn = domain::make_fqdn(domain.name, subdomain.name);
    try {
        co_await subdomain_loop(domain, subdomain, services);
    } catch (const coro::Cancelled&) {
        throw;
    } catch (const std::exception& error) {
        YLOG_ERROR(services.logger, "Subdomain loop for {} died: {}", fqdn, error.what());
    } catch (...) {
        YLOG_ERROR(services.logger, "Subdomain loop for {} died with a non-standard exception", fqdn);
    }
}

/// The work phase: every subdomain loop runs in one supervisor group until a
/// watcher cancels the group's scope or every loop has died of a defect.
///
/// The group's own scope doubles as the work scope — cancelling it reaches
/// every loop at its checkpoints. A signal that arrived before the group
/// existed is honoured at entry, so no shutdown is lost.
coro::Task<void> run_work(const std::shared_ptr<const domain::RuntimeConfig>& config, const Services& services,
                          ShutdownState& shutdown) {
    // The group may propagate a body defect or ancestor cancellation. Clear
    // the borrowed scope on every body exit while the scope is still alive.
    struct ResetWorkScope {
        ShutdownState& state;

        ~ResetWorkScope() noexcept { state.work_scope = nullptr; }
    };

    co_await coro::supervisor_group([&](coro::TaskGroup& work) -> coro::Task<void> {
        shutdown.work_scope = &work.scope();
        const ResetWorkScope reset{shutdown};
        if (shutdown.requested) {
            work.cancel();
        }
        for (const auto& domain : config->domains) {
            for (const auto& subdomain : domain.subdomains) {
                work.spawn(guarded_subdomain_loop(domain, subdomain, services));
            }
        }
        // Keep the borrow published until every loop has ended, then clear it
        // before the group scope is destroyed and a watcher can run again.
        while (co_await work.next<void>()) {
        }
        co_return;
    });
}

}  // namespace

coro::Task<int> run_root(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    std::size_t task_count = 0;
    for (const auto& domain : config->domains) {
        task_count += domain.subdomains.size();
    }
    YLOG_INFO(services.logger, "Run root initialised with {} tasks", task_count);

    ShutdownState shutdown;
    co_await coro::supervisor_group([&](coro::TaskGroup& root) -> coro::Task<void> {
        const auto gateway = services.make_gateway(root);
        const Services svc{services.resolver, services.ip_source, *gateway, services.logger};

        // Watchers live in the root scope, not the work scope, so the shutdown
        // they trigger does not end them; the second SIGINT can still be caught.
        // They are cancelled explicitly by root.cancel() once the work is done.
        root.spawn(watch_signal(SIGINT, true, shutdown, services.logger, services.drain_logs));
        root.spawn(watch_signal(SIGTERM, false, shutdown, services.logger, services.drain_logs));

        co_await run_work(config, svc, shutdown);

        // The work is finished: stop the watchers so the root group can join.
        root.cancel();
        co_return;
    });
    YLOG_INFO(services.logger, "All tasks drained, shutting down");

    if (task_count > 0 && !shutdown.requested) {
        // Every loop is dead and no shutdown was asked for: the daemon is
        // functionally dead. Exit with a failure status so a supervisor
        // (Restart=on-failure) starts it again instead of leaving a husk.
        YLOG_CRITICAL(services.logger, "All subdomain loops have died; exiting with a failure status");
        co_return EXIT_FAILURE;
    }
    co_return EXIT_SUCCESS;
}

}  // namespace app
