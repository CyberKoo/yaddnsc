//
// app — one coroutine update cycle (the coroutine UpdateWorkflow).
//
// Not to be confused with domain::UpdateTask (domain/update/update_task.h):
// that struct is the data describing one record's work, while this task is
// the coroutine that executes it.
//

#ifndef YADDNSC_APPLICATION_TASKS_UPDATE_TASK_H
#define YADDNSC_APPLICATION_TASKS_UPDATE_TASK_H

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
/// values (logged in place with compatibility-stable wording); allocation failure is a defect and propagates, because
/// it is not a retryable condition.
[[nodiscard]] coro::Task<UpdateCycleOutcome> update_task(const domain::UpdateTask& task, const Services& services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_TASKS_UPDATE_TASK_H
