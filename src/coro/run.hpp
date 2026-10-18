//
// Coroutine runtime — entry point.
//
// The loop runs on the calling thread and returns after root completion and
// submitted worker drain, keeping cross-thread inbox delivery alive throughout.
// Tests may pass a loop built on a manual clock; time control is a loop property
// and never penetrates the task API.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_RUN_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_RUN_HPP

#include <cassert>
#include <exception>
#include <type_traits>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/task_promise.hpp"  // IWYU pragma: export
#include "coro/loop.h"
#include "coro/task.hpp"

namespace coro {

/// Run `task` to completion on `loop`, which runs on this thread.
///
/// Ownership: takes over the root frame. The root's record and result slot are
/// locals of this call, so they stay at a fixed address and cover the whole run:
/// the loop's reap transaction retires the record, publishes the result, and
/// only then can this function read it.
/// Lifetime: the loop must outlive the call; the loop's clock must outlive the
/// loop. The root scope created here is the ancestor of every scope the task
/// creates.
/// Failure: rethrows the root task's defect or cancellation; T's move
/// constructor may throw when the result is moved out. A task that never
/// completes never returns. After the root retires the loop still pumps worker
/// completions before returning; abandon ends an await, not the runtime's
/// ownership of submitted work.
/// Failure taxonomy: an operational poll() failure (ENOMEM) is not fatal —
/// the loop paces itself, and sustained impairment fails fd waits with the
/// poll errno (loop.h). What stays fatal is a dispatch failure: an exception
/// escaping loop dispatch, or a defect in the loop's own call construction
/// (EINVAL/EFAULT from poll), calls std::terminate — parked frames and lost
/// callbacks cannot be safely unwound. Use a fresh loop for each run: a run
/// ends by draining everything it started, and a loop that hosted one session
/// is not re-armed for another.
/// Thread safety: must be called from the loop's own thread.
template<typename T>
[[nodiscard]] T run(Loop& loop, Task<T> task) {
    assert(task.valid() && "coro::run requires a valid task");
    CancelScope root;
    // Preparation: the stable storage the root's execution needs. Nothing below
    // this point can fail, so the task is released only once the record and its
    // result slot exist at their final addresses.
    detail::ExecutionRecord record;
    detail::ResultState<T> result;
    detail::TaskAccess::start_root(task, record, result, loop, root);
    try {
        loop.run();
    } catch (...) {
        // A dispatch failure can lose inbox completions or a detached ready
        // batch. Unwinding parked frames cannot safely recover that state.
        std::terminate();
    }
    // run() returns only once the session is complete, and the registry empties
    // exactly when the root's whole subtree retired. A root that is not
    // retired here means the loop left with a live execution.
    assert(record.phase() == detail::ExecutionPhase::RETIRED && "run() returned with a live root record");
    if (record.phase() != detail::ExecutionPhase::RETIRED) {
        std::terminate();
    }
    assert(result.ready() && "root retired without publishing its result");
    if (!result.ready()) {
        std::terminate();
    }
    result.commit_consumed();
    if (result.content() != detail::ResultContent::VALUE) {
        std::rethrow_exception(result.error());
    }
    if constexpr (!std::is_void_v<T>) {
        return result.take_value();
    }
}

/// Run `task` to completion on a fresh loop with the system clock.
///
/// Same contract as above; use the Loop overload when a manual clock or a
/// specific worker count is needed. The loop is constructed inside this call
/// and owns its scheduling and wake state independently of other loops.
template<typename T>
[[nodiscard]] T run(Task<T> task) {
    Loop loop;
    return run(loop, std::move(task));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_RUN_HPP