//
// app — one coroutine update cycle (the coroutine UpdateWorkflow).
//

#ifndef YADDNSC_APPLICATION_RUN_UPDATE_CYCLE_H
#define YADDNSC_APPLICATION_RUN_UPDATE_CYCLE_H

#include <chrono>

#include <expected>

#include "coro/task.hpp"
#include "domain/error/error.h"

namespace domain {
enum class UpdateDecision;
struct UpdateTask;
}  // namespace domain

namespace app {
struct Services;

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
