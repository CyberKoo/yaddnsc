//
// app — one coroutine update cycle (the coroutine UpdateWorkflow).
//

#ifndef YADDNSC_APPLICATION_UPDATE_ONCE_H
#define YADDNSC_APPLICATION_UPDATE_ONCE_H

#include <chrono>
#include <expected>

#include "application/services.h"
#include "domain/error/error.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// Budget for reading the current DNS records inside one cycle.
///
/// The coroutine exchange layer has no I/O timeout, so the read must be bounded
/// by a scope; a resolver that never answers has to surface in seconds rather
/// than hold the whole cycle to UPDATE_BUDGET (30s). 4s is well under that
/// ceiling and matches the legacy per-query budgets (UDP 1s + TCP fallback 1s).
inline constexpr std::chrono::seconds DNS_READ_BUDGET{4};

/// What one executed cycle decided (and did).
struct UpdateCycleResult {
    domain::UpdateDecision decision;
};

/// The cycle's outcome: the decision, or the failure that ended it early.
using UpdateOnceOutcome = std::expected<UpdateCycleResult, domain::UpdateError>;

/// Execute one update cycle for `task`: resolve the local address, read the
/// current records (unless forced), decide, and hand the change to the gateway.
///
/// Cancellation: every port await is a scope checkpoint; a cancelled scope makes
/// the ports return their CANCELLED values, which this maps to
/// UpdateError::CANCELLED. Failure: expected failures are UpdateError values
/// (logged in place with the legacy wording); allocation failure is a defect and
/// propagates, because it is not a retryable condition.
[[nodiscard]] coro::Task<UpdateOnceOutcome> update_once(const domain::UpdateTask& task, const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_UPDATE_ONCE_H
