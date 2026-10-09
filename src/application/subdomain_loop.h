//
// app — the per-subdomain long-lived scheduling coroutine (design §5).
//
// The central scheduler is dissolved: there is no queue, no runner and no
// executor. One coroutine per subdomain carries its own ordering state
// (last force-update time, interval, backoff) as frame locals and sleeps between
// cycles.
//

#ifndef YADDNSC_APPLICATION_SUBDOMAIN_LOOP_H
#define YADDNSC_APPLICATION_SUBDOMAIN_LOOP_H

#include <chrono>

#include "application/run_update_cycle.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/coro/time.h"

namespace app {

struct Services;

/// Drive one subdomain forever: update, sleep, repeat, until the enclosing
/// scope is cancelled.
///
/// The force-update interval: the first cycle is forced when the interval is
/// positive (last_force_update starts one interval in the past), and each cycle
/// whose elapsed time reaches the interval is forced, which latches
/// last_force_update to the current time.
///
/// Cancellation: surfaces only through await results, never by polling scope
/// state. A cancelled enclosing scope throws `coro::Cancelled` from the
/// in-flight cycle or pacing sleep, and
/// the loop exits instead of starting another cycle.
/// Failure: an escaping defect (e.g. allocation failure) aborts this subdomain
/// only; the runner's supervisor group keeps the siblings running.
[[nodiscard]] coro::Task<void> subdomain_loop(const domain::DomainConfig& domain,
                                              const domain::SubdomainConfig& subdomain,
                                              const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_SUBDOMAIN_LOOP_H
