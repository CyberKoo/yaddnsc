// Coroutine runtime — the scope combinator body.
//
// One coroutine serves all four public combinators: they differ only in which
// ScopeSpec fields they set, and the shielded flag is what separates
// non_cancellable() from a plain child scope. scope.hpp includes this header so
// the template definition is available at instantiation. This header includes
// scope_outcome.hpp for the public result it builds, not scope.hpp, keeping the
// include graph acyclic.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SCOPE_RUNNER_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SCOPE_RUNNER_HPP

#include <exception>
#include <type_traits>
#include <utility>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/context.h"
#include "infrastructure/coro/detail/result_box.hpp"
#include "infrastructure/coro/detail/task_promise.h"
#include "infrastructure/coro/detail/timer_node.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/scope_outcome.hpp"
#include "infrastructure/coro/task.hpp"

namespace coro::detail {

/// Result type produced by a combinator body: `fn(scope)` returns Task<T>.
template<typename Fn>
using BodyResult = typename std::invoke_result_t<Fn&, CancelScope&>::ValueType;

/// What a combinator asks run_scoped() to set up.
struct ScopeSpec {
    bool use_timeout = false;
    bool use_deadline = false;
    Duration timeout{};
    TimePoint deadline{};
    /// Shielded scopes ignore ancestor cancellation (non_cancellable).
    bool shielded = false;
};

/// Runs `fn`'s body in a fresh child scope and reports how it ended.
///
/// Lifetime: the scope and the timer live in this coroutine's frame, which
/// outlives the body because the body is joined before the outcome is built;
/// the body's awaits therefore always have a valid scope to park on.
template<typename Fn>
Task<ScopeOutcome<BodyResult<Fn>>> run_scoped(Fn fn, ScopeSpec spec) {
    using R = BodyResult<Fn>;
    const Context context = co_await GetContext{};
    CancelScope scope{context.scope, spec.shielded};
    TimerNode timer{};
    if (spec.use_timeout || spec.use_deadline) {
        const TimePoint deadline = spec.use_deadline ? spec.deadline : context.loop->now() + spec.timeout;
        LoopAccess::add_timer(*context.loop, timer, deadline, &ScopeAccess::timeout_action, &scope);
    }

    ResultBox<R> box;
    std::exception_ptr error;
    bool cancelled = false;
    {
        try {
            Task<R> body = fn(scope);
            TaskAccess::bind_context(body, *context.loop, scope);
            if constexpr (std::is_void_v<R>) {
                co_await std::move(body);
            } else {
                box.value.emplace(co_await std::move(body));
            }
        } catch (const Cancelled& cancellation) {
            cancelled = true;
            if (!ScopeAccess::absorbs(scope, cancellation)) {
                error = std::current_exception();
            }
        } catch (...) {
            error = std::current_exception();
        }
    }
    LoopAccess::remove_timer(*context.loop, timer);

    ScopeOutcome<R> outcome;
    outcome.timed_out = scope.timed_out();
    outcome.cancelled = scope.cancelled();
    try {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
        scope.throw_if_cancelled();
    } catch (const Cancelled& cancellation) {
        if (!ScopeAccess::absorbs(scope, cancellation)) {
            scope.throw_if_cancelled();
            throw;
        }
        cancelled = true;
    }
    if constexpr (std::is_void_v<R>) {
        outcome.completed = !cancelled;
    } else {
        if (!cancelled) {
            outcome.value = std::move(box.value);
        }
    }
    co_return outcome;
}

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SCOPE_RUNNER_HPP