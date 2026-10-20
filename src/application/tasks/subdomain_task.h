//
// app — the per-subdomain long-lived task coroutine (design §5).
//
// The central scheduler is dissolved: there is no queue and no executor. One
// task coroutine per subdomain carries its own ordering state (last
// force-update completion time, interval, cycle start) as frame locals and
// sleeps between cycles.
//

#ifndef YADDNSC_APPLICATION_TASKS_SUBDOMAIN_TASK_H
#define YADDNSC_APPLICATION_TASKS_SUBDOMAIN_TASK_H

#include "coro/task.hpp"

namespace domain {
struct DomainConfig;
struct SubdomainConfig;
}  // namespace domain

namespace app {

struct Services;

/// Drive one subdomain until the enclosing scope is cancelled or the next
/// update deadline cannot be represented by the clock.
///
/// The force-update interval: the first cycle is forced when the interval is
/// positive (the last forced publication starts one interval in the past), and
/// each cycle whose elapsed time reaches the interval is forced. Only a
/// successful forced publication commits the completion time; a failed or
/// timed-out forced cycle stays due and is retried as forced.
///
/// A non-positive configured interval is clamped to the build-time minimum —
/// defence in depth for a RuntimeConfig built without static validation.
///
/// Cancellation: surfaces only through await results, never by polling scope
/// state. A cancelled enclosing scope throws `coro::Cancelled` from the
/// in-flight cycle, the pacing sleep or the retry wait, and the task exits
/// instead of starting another cycle.
/// Failure: a defect escaping an update cycle (e.g. an allocation failure) is
/// logged and retried after one full update interval. The next cycle is forced
/// when force updates are enabled, preserving the former restart policy. If the
/// clock cannot represent a future deadline, the task logs an error and returns.
[[nodiscard]] coro::Task<void> subdomain_task(const domain::DomainConfig& domain,
                                              const domain::SubdomainConfig& subdomain, const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_TASKS_SUBDOMAIN_TASK_H
