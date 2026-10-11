//
// app — the run root (implementation).
//

#include "run_root.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <optional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <string>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <unistd.h>

#include "application/log_macros.h"
#include "application/services.h"
#include "application/subdomain_loop.h"
#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/group.hpp"
#include "coro/signal.hpp"
#include "coro/sleep.hpp"
#include "coro/task_group.hpp"
#include "domain/config/runtime_config.h"
#include "domain/fqdn.h"

#include "min_update_interval.h"

namespace app {
class LoggerPort;

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
/// supervisor group keeps the siblings alive, and a defect never retires the
/// subdomain: the legacy scheduler mapped a workflow exception to an error
/// value and the queue retried on the next interval, so a dead loop is
/// restarted after one interval. The restart sleep doubles as the shutdown
/// checkpoint — a cancelled wait throws `Cancelled` and unwinds.
coro::Task<void> guarded_subdomain_loop(const domain::DomainConfig& domain, const domain::SubdomainConfig& subdomain,
                                        const Services& services) {
    const std::string fqdn = domain::make_fqdn(domain.name, subdomain.name);

    // Defence in depth for a RuntimeConfig built without static validation: a
    // non-positive interval would turn the pacing sleep into a hot spin.
    domain::SubdomainConfig effective = subdomain;
    if (effective.update_interval <= 0) {
        YLOG_WARN(services.logger,
                  "Subdomain \"{}\" has a non-positive update interval ({}); clamping to the minimum {}s", fqdn,
                  subdomain.update_interval, YADDNSC_MIN_UPDATE_INTERVAL);
        effective.update_interval = YADDNSC_MIN_UPDATE_INTERVAL;
    }

    for (;;) {
        try {
            co_await subdomain_loop(domain, effective, services);
            co_return;
        } catch (const coro::Cancelled&) {
            YLOG_DEBUG(services.logger, "Subdomain loop for \"{}\" stopped", fqdn);
            throw;
        } catch (const std::exception& error) {
            YLOG_ERROR(services.logger, "Subdomain loop for \"{}\" died: {}; restarting after the update interval",
                       fqdn, error.what());
        } catch (...) {
            YLOG_ERROR(
                services.logger,
                "Subdomain loop for \"{}\" died with a non-standard exception; restarting after the update interval",
                fqdn);
        }
        co_await coro::sleep_for(std::chrono::seconds(effective.update_interval));
    }
}

/// The work phase: every subdomain loop runs in one supervisor group until a
/// watcher cancels the group's scope. A loop that dies of a defect is
/// restarted by its guard, so the phase ends through cancellation only.
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
        // This phase needs a drain, not cancellable result collection. Keep
        // reset outside the child body so even partial-launch defects cancel
        // and join every loop before the published scope borrow is cleared.
        co_await work.with_children([&]() -> coro::Task<void> {
            if (shutdown.requested) {
                work.cancel();
                co_return;  // no work should start after a startup shutdown
            }
            work.scope().throw_if_cancelled();
            for (const auto& domain : config->domains) {
                for (const auto& subdomain : domain.subdomains) {
                    work.spawn(guarded_subdomain_loop(domain, subdomain, services));
                }
            }
        });
        co_return;
    });
}

}  // namespace

coro::Task<int> run_root(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services) {
    std::size_t task_count = 0;
    for (const auto& domain : config->domains) {
        task_count += domain.subdomains.size();
    }
    YLOG_INFO(services.logger, "Starting update loops for {} records", task_count);

    ShutdownState shutdown;
    if (services.startup_signals) {
        // Signals caught before the loop existed count exactly as if the
        // watcher had seen them: the shutdown is honoured at work entry, and a
        // repeat SIGINT still escalates to the forced exit.
        const auto seen = services.startup_signals();
        shutdown.sigint_count = seen.sigint;
        shutdown.requested = seen.sigint > 0 || seen.sigterm > 0;
    }
    co_await coro::supervisor_group([&](coro::TaskGroup& root) -> coro::Task<void> {
        const auto gateway = services.make_gateway(root);
        const Services svc{services.resolver, services.ip_source, *gateway, services.logger};

        // gateway/svc stay in this outer body while with_children drains both
        // watchers and bridge requests, including defect/cancellation exits.
        co_await root.with_children([&]() -> coro::Task<void> {
            // Watchers outlive the work scope so a second SIGINT can escalate.
            root.spawn(watch_signal(SIGINT, true, shutdown, services.logger, services.drain_logs));
            root.spawn(watch_signal(SIGTERM, false, shutdown, services.logger, services.drain_logs));
            co_await run_work(config, svc, shutdown);
            root.cancel();
        });
        co_return;
    });
    YLOG_INFO(services.logger, "All tasks drained, shutting down");

    if (task_count > 0 && !shutdown.requested) {
        // Defensive: the loop guards restart defects, so a normal return here
        // means every guard retired without a shutdown request — which no
        // current path does. If that ever changes, exit with a failure status
        // so a supervisor (Restart=on-failure) starts the daemon again
        // instead of leaving a husk.
        YLOG_CRITICAL(services.logger, "All subdomain loops have died; exiting with a failure status");
        co_return EXIT_FAILURE;
    }
    co_return EXIT_SUCCESS;
}

}  // namespace app
