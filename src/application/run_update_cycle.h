//
// app — one coroutine update cycle (the coroutine UpdateWorkflow).
//

#ifndef YADDNSC_APPLICATION_RUN_UPDATE_CYCLE_H
#define YADDNSC_APPLICATION_RUN_UPDATE_CYCLE_H

#include <chrono>

#include <expected>

#include "application/services.h"
#include "domain/error/error.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// Overall budget for one update cycle; narrower stage budgets below allow
/// the cycle to handle a stalled source or DNS read before this final bound.
inline constexpr std::chrono::seconds UPDATE_BUDGET{30};

/// Budget for reading the current DNS records inside one cycle.
///
/// The coroutine exchange layer has no I/O timeout, so the read must be bounded
/// by a scope; a resolver that never answers has to surface in seconds rather
/// than hold the whole cycle to UPDATE_BUDGET (30s). 4s is well under that
/// ceiling and matches the legacy per-query budgets (UDP 1s + TCP fallback 1s).
inline constexpr std::chrono::seconds DNS_READ_BUDGET{4};

/// Budget for resolving the local address inside one cycle.
///
/// The coroutine HTTP layer has no I/O timeout, so the bound is composed here,
/// the same way as DNS_READ_BUDGET: a provider that accepts and never answers
/// must fail this cycle in seconds instead of holding it to UPDATE_BUDGET
/// (30s). 10s covers the legacy per-operation budgets (connect + send + read
/// at 5s each could never all stall at once for a working endpoint).
inline constexpr std::chrono::seconds IP_SOURCE_BUDGET{10};

/// What one executed cycle decided (and did).
struct UpdateCycleResult {
    domain::UpdateDecision decision;
};

/// The cycle's outcome: the decision, or the failure that ended it early.
using UpdateCycleOutcome = std::expected<UpdateCycleResult, domain::UpdateError>;

/// Execute one update cycle for `task`: resolve the local address, read the
/// current records (unless forced), decide, and hand the change to the gateway.
///
/// Cancellation propagates as `coro::Cancelled` from port checkpoints. Failure: expected failures are UpdateError
/// values (logged in place with the legacy wording); allocation failure is a defect and propagates, because it is not a
/// retryable condition.
[[nodiscard]] coro::Task<UpdateCycleOutcome> run_update_cycle(const domain::UpdateTask& task, const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_RUN_UPDATE_CYCLE_H
