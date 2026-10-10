// Coroutine runtime — the shared body of task_group() and supervisor_group().
//
// group.hpp includes this header so the template definition is available at
// instantiation. This header includes task_group.hpp for the public types it
// builds, not group.hpp, keeping the include graph acyclic.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_RUNNER_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_RUNNER_HPP

#include <exception>

#include <coroutine>

#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/context.h"
#include "infrastructure/coro/detail/group_state.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/coro/task_group.hpp"

namespace coro::detail {

template<typename Fn, bool Supervisor>
Task<void> run_group(Fn fn) {
    const Context context = co_await GetContext{};
    GroupState state{context.loop, context.scope, Supervisor};
    TaskGroup group{&state};

    Task<void> body = fn(group);
    TaskAccess::bind_context(body, *context.loop, state.scope);
    auto body_handle = TaskAccess::release(body);
    co_await StartAndAwait{&body_handle.promise()};
    const std::exception_ptr body_error = body_handle.promise().error;

    if (body_error && !body_handle.promise().cancellation) {
        ScopeAccess::cancel(state.scope, CancelCause::REQUESTED);
    }
    while (!state.all_done()) {
        co_await AllDoneAwaitable{&state};
    }
    const std::exception_ptr child_error = state.first_error;
    state.reap();
    // The body frame stays alive until every child it spawned has been reaped.
    body_handle.destroy();

    if constexpr (!Supervisor) {
        if (child_error) {
            std::rethrow_exception(child_error);
        }
    }
    try {
        if (body_error) {
            std::rethrow_exception(body_error);
        }
        state.scope.throw_if_cancelled();
    } catch (const Cancelled& cancellation) {
        if (!ScopeAccess::absorbs(state.scope, cancellation)) {
            state.scope.throw_if_cancelled();
            throw;
        }
    }
    co_return;
}

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_RUNNER_HPP
