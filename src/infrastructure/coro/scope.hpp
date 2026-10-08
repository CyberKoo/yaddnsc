//
// Coroutine runtime — cancel-scope combinators.
//
// Four combinators cover every cancellation trigger: a duration timeout, an
// absolute deadline, a scope the caller cancels by hand, and a shield that
// protects cleanup from an outer cancellation.
//
// A combinator runs its body in a child scope; every cancellable await in the
// body returns `unexpected(operation_canceled)` once the scope is cancelled, so
// the body decides how to finish. The scope absorbs its own cancellation: the
// combinator returns normally (or with the body's defect), and the caller reads
// `timed_out()` / `cancelled()` from the returned outcome.
//

#ifndef YADDNSC_CORO_SCOPE_HPP
#define YADDNSC_CORO_SCOPE_HPP

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/result_box.hpp"
#include "infrastructure/coro/task.hpp"

namespace coro {

/// Body result plus the scope state that produced it.
///
/// Ownership: owns the body's value. A combinator always waits for its body, so
/// on a normal return `value` is engaged; a defect from the body is rethrown
/// instead of being stored.
/// Failure: reading `*outcome` when `has_value()` is false is a precondition
/// violation.
template<typename T>
struct ScopeOutcome {
    std::optional<T> value;
    /// The combinator's own deadline fired.
    bool timed_out = false;
    /// The combinator's scope was cancelled (by itself or by an outer scope).
    bool cancelled = false;

    /// True when the body produced a value.
    [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }

    /// Same as has_value(): the body completed and yielded a value.
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] T& operator*() noexcept { return *value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] const T& operator*() const noexcept { return *value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] T* operator->() noexcept { return &*value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] const T* operator->() const noexcept { return &*value; }
};

/// void specialization: completion is the only observable result.
template<>
struct ScopeOutcome<void> {
    bool completed = false;
    bool timed_out = false;
    bool cancelled = false;

    [[nodiscard]] explicit operator bool() const noexcept { return completed; }
};

namespace detail {

/// Result type produced by a combinator body: `fn(scope)` returns Task<T>.
template<typename Fn>
using BodyResult = typename std::invoke_result_t<Fn&, CancelScope&>::value_type;

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
        context.loop->add_timer(timer, deadline, &CancelScope::timeout_action, &scope);
    }

    ResultBox<R> box;
    std::exception_ptr error;
    {
        Task<R> body = fn(scope);
        body.bind_context(*context.loop, scope);
        try {
            if constexpr (std::is_void_v<R>) {
                co_await std::move(body);
            } else {
                box.value.emplace(co_await std::move(body));
            }
        } catch (...) {
            error = std::current_exception();
        }
    }
    context.loop->remove_timer(timer);

    ScopeOutcome<R> outcome;
    outcome.timed_out = scope.timed_out();
    outcome.cancelled = scope.cancelled();
    if (error != nullptr) {
        std::rethrow_exception(error);
    }
    if constexpr (std::is_void_v<R>) {
        outcome.completed = true;
    } else {
        outcome.value = std::move(box.value);
    }
    co_return outcome;
}

}  // namespace detail

/// Run `fn` in a scope with a duration timeout `timeout`.
///
/// `fn` receives the scope by reference (so it can read `timed_out()` inside
/// the body) and returns the body task. On expiry the scope is cancelled, the
/// body's awaits return `operation_canceled`, and the returned outcome reports
/// `timed_out()`; the cancellation does not leak to the caller's scope.
/// Failure: allocation may throw; a body defect is rethrown.
template<typename Fn>
[[nodiscard]] auto with_timeout(Duration timeout, Fn fn) {
    return detail::run_scoped(std::move(fn), detail::ScopeSpec{.use_timeout = true, .timeout = timeout});
}

/// Run `fn` in a scope with an absolute deadline on the loop clock.
///
/// Same contract as with_timeout, with the deadline expressed absolutely.
template<typename Fn>
[[nodiscard]] auto with_deadline(TimePoint deadline, Fn fn) {
    return detail::run_scoped(std::move(fn), detail::ScopeSpec{.use_deadline = true, .deadline = deadline});
}

/// Run `fn` in a child scope the caller may cancel through the reference it
/// receives. The scope absorbs only its own cancellation.
///
/// This is how a sibling cancels a peer by hand: it stores the scope pointer
/// and calls `cancel()`.
template<typename Fn>
[[nodiscard]] auto with_cancel_scope(Fn fn) {
    return detail::run_scoped(std::move(fn), detail::ScopeSpec{});
}

/// Run `fn` in a shielded scope: an outer cancellation does not reach it.
///
/// Use for cleanup/drain that must complete even while the surrounding scope is
/// being cancelled. Failure and cancellation reporting are as above; a shield
/// never reports `cancelled()` for an outer cancel.
template<typename Fn>
[[nodiscard]] auto non_cancellable(Fn fn) {
    return detail::run_scoped(std::move(fn), detail::ScopeSpec{.shielded = true});
}

}  // namespace coro

#endif  // YADDNSC_CORO_SCOPE_HPP
