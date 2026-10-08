//
// app — the run root: spawn one coroutine per subdomain, wire shutdown.
//
// There is no scheduler loop and no "loop-before" stage: the run is a structured
// supervisor group whose children are the per-subdomain loops, plus a signal
// watcher that cancels the work scope on SIGINT/SIGTERM.
//

#ifndef YADDNSC_APPLICATION_CORO_RUN_SCHEDULER_H
#define YADDNSC_APPLICATION_CORO_RUN_SCHEDULER_H

#include <memory>

#include "application/coro/services.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// Run the scheduler until a shutdown signal cancels it.
///
/// Structure (design §5): one `subdomain_loop` per configured subdomain is
/// spawned into a supervisor group, so a single subdomain's failure is reported
/// and never cancels its siblings. The signal watchers live in the root group
/// but outside the cancellable work scope, so they survive the shutdown they
/// trigger and can catch the escalating second SIGINT. When the work scope is
/// cancelled, the subdomain loops wake and exit; the root then cancels the
/// watchers and the group joins everything.
///
/// The gateway is built through `services.make_gateway(root)` so the driver
/// bridge coroutines are structured children of the root group.
///
/// Returns EXIT_SUCCESS after a graceful shutdown. Second SIGINT terminates the
/// process (128 + SIGINT), matching the legacy SignalWatcher.
[[nodiscard]] coro::Task<int> run_scheduler(std::shared_ptr<const domain::RuntimeConfig> config,
                                            RuntimeServices services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_CORO_RUN_SCHEDULER_H
