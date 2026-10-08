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
#include <cstddef>
#include <memory>

#include "application/update_once.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/task.hpp"

namespace app {

struct Services;

/// Overall budget for one update cycle, enforced by with_timeout. It replaces
/// every legacy I/O timeout parameter: the old transport defaulted each
/// connect/read/write to a few seconds, and the legacy workflow chained an IP
/// source fetch, a DNS read and an HTTP driver call, so ~30s bounds the whole
/// cycle generously without letting a wedged provider stall a subdomain.
inline constexpr std::chrono::seconds UPDATE_BUDGET{30};

/// The delay before the next cycle of a subdomain loop.
///
/// Mirrors the legacy scheduler: the update interval advances the schedule, and
/// a provider-supplied retry_after overrides it (the legacy request_retry →
/// reschedule path). A zero retry_after leaves the interval in place.
[[nodiscard]] coro::Duration next_delay(const UpdateOnceOutcome& outcome, int update_interval) noexcept;

/// Drive one subdomain forever: update, sleep, repeat, until the enclosing
/// scope is cancelled.
///
/// The force-update interval: the first cycle is forced when the interval is
/// positive (last_force_update starts one interval in the past), and each cycle
/// whose elapsed time reaches the interval is forced, which latches
/// last_force_update to the current time.
///
/// Cancellation: a checkpoint. The loop re-checks its scope before every cycle
/// and wakes early from its sleep when the scope is cancelled, so shutdown does
/// not run one more update.
/// Failure: an escaping defect (e.g. allocation failure) aborts this subdomain
/// only; the runner's supervisor group keeps the siblings running.
[[nodiscard]] coro::Task<void> subdomain_loop(std::shared_ptr<const domain::RuntimeConfig> config,
                                              std::size_t domain_index, std::size_t subdomain_index,
                                              const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_SUBDOMAIN_LOOP_H
