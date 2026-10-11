// Coroutine runtime — the shared body of task_group() and supervisor_group().
//
// group.hpp includes this header so the template definition is available at
// instantiation. This header includes task_group.hpp for the public types it
// builds, not group.hpp, keeping the include graph acyclic.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_RUNNER_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_RUNNER_HPP

#include <exception>

#include <coroutine>

#include "coro/detail/access.h"
#include "coro/detail/context.h"
#include "coro/detail/group_state.h"
#include "coro/group_error.h"
#include "coro/task.hpp"
#include "coro/task_group.hpp"

namespace coro::detail {

template<typename Fn, bool Supervisor>
Task<void> run_group(Fn fn) {
    const Context context = co_await GetContext{};
    GroupState state{context.loop, context.scope, Supervisor};
    TaskGroup group{&state};

    std::coroutine_handle<TaskPromise<void>> body_handle{};
    std::exception_ptr body_error;
    bool body_cancelled = false;
    try {
        Task<void> body = fn(group);
        TaskAccess::bind_context(body, *context.loop, state.scope);
        body_handle = TaskAccess::release(body);
        co_await StartAndAwait{&body_handle.promise()};
        body_error = body_handle.promise().error;
        body_cancelled = body_handle.promise().cancellation;
    } catch (const Cancelled&) {
        body_error = std::current_exception();
        body_cancelled = true;
    } catch (...) {
        body_error = std::current_exception();
    }
    state.lifetime->group = nullptr;  // close ingress before exit drain

    if (body_error && !body_cancelled) {
        ScopeAccess::cancel(state.scope, CancelCause::REQUESTED);
    }
    while (!state.all_done()) {
        co_await AllDoneAwaitable{&state};
    }
    // Outcome collection can allocate; keep both owners exception-safe.
    const FrameOwner body_owner{body_handle};
    const std::exception_ptr child_error = state.first_error;
    std::vector<std::exception_ptr> unobserved;
    try {
        if constexpr (Supervisor) {
            unobserved = state.unobserved_errors();
        }
    } catch (...) {
        // Even aggregation allocation failure must reap children before the
        // retained body parameters/closure are released.
        state.reap();
        throw;
    }
    state.reap();

    if constexpr (!Supervisor) {
        if (child_error) {
            std::rethrow_exception(child_error);
        }
    } else {
        if (!unobserved.empty()) {
            if (body_error && !body_cancelled) {
                unobserved.push_back(body_error);
            }
            throw GroupError{std::move(unobserved)};
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
