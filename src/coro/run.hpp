//
// Coroutine runtime — entry point.
//
// The loop runs on the calling thread and returns when the root task completes.
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
#include "coro/detail/frame.h"
#include "coro/detail/task_promise.h"  // IWYU pragma: export
#include "coro/loop.h"
#include "coro/task.hpp"

namespace coro {

/// Run `task` to completion on `loop`, which runs on this thread.
///
/// Ownership: takes over the root frame and destroys it before returning.
/// Lifetime: the loop must outlive the call; the loop's clock must outlive the
/// loop. The root scope created here is the ancestor of every scope the task
/// creates.
/// Failure: rethrows the root task's defect; T's move constructor may throw
/// when the result is moved out. A task that never completes never returns.
/// An exception escaping loop dispatch is fatal (std::terminate): parked
/// frames and lost callbacks cannot be safely unwound. Use a fresh loop for
/// each run; externally stopping a live root violates this contract.
/// Thread safety: must be called from the loop's own thread.
template<typename T>
[[nodiscard]] T run(Loop& loop, Task<T> task) {
    assert(task.valid() && "coro::run requires a valid task");
    assert(!loop.stopped() && "coro::run requires a fresh loop");
    CancelScope root;
    auto handle = detail::TaskAccess::release(task);
    detail::PromiseBase& promise = handle.promise();
    promise.loop = &loop;
    promise.scope = &root;
    promise.context_bound = true;
    promise.is_root = true;
    detail::LoopAccess::schedule(loop, promise);
    try {
        loop.run();
    } catch (...) {
        // A dispatch failure can lose inbox completions or a detached ready
        // batch. Unwinding parked frames cannot safely recover that state.
        std::terminate();
    }
    if (!handle.done()) {
        // An external stop cannot substitute for structured root completion.
        std::terminate();
    }
    const detail::FrameOwner owner{handle};
    if (handle.promise().error) {
        std::rethrow_exception(handle.promise().error);
    }
    if constexpr (std::is_void_v<T>) {
        return;
    } else {
        return std::move(*handle.promise().value);
    }
}

/// Run `task` to completion on a fresh loop with the system clock.
///
/// Same contract as above; use the Loop overload when a manual clock or a
/// specific worker count is needed.
template<typename T>
[[nodiscard]] T run(Task<T> task) {
    Loop loop;
    return run(loop, std::move(task));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_RUN_HPP