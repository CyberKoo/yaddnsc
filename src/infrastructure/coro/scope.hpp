//
// Coroutine runtime — cancel-scope combinators.
//
// Four combinators cover every cancellation trigger: a duration timeout, an
// absolute deadline, a scope the caller cancels by hand, and a shield that
// protects cleanup from an outer cancellation.
//
// A combinator runs its body in a child scope; every cancellable await in the
// body throws `coro::Cancelled` once the scope is cancelled. The scope absorbs its own cancellation: the
// combinator returns normally (or with the body's defect), and the caller reads
// `timed_out` / `cancelled` from the returned outcome.
//
// current_scope() is the one way a body reaches the scope it is already running
// in — for cancelling it from a sibling, or converting an ABI abort into
// coroutine cancellation. It yields the scope and nothing else: no loop, no
// clock, no frame.
//
// This header exposes the four combinators and current_scope(). The outcome type lives in
// scope_outcome.hpp and the body they share in detail/scope_runner.hpp.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_SCOPE_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_SCOPE_HPP

#include <utility>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/context_awaitables.h"
#include "infrastructure/coro/detail/scope_runner.hpp"
#include "infrastructure/coro/scope_outcome.hpp"
#include "infrastructure/coro/time.h"

namespace coro {

/// Run `fn` in a scope with a duration timeout `timeout`.
///
/// `fn` takes no arguments and returns the body task. On expiry the scope is
/// cancelled, the body's awaits throw `coro::Cancelled`, and the returned
/// outcome reports `timed_out`; own cancellation does not leak to the caller.
/// Ancestor cancellation propagates instead of returning an outcome.
/// Failure: allocation may throw; a body defect is rethrown.
template<typename Fn>
[[nodiscard]] auto with_timeout(Duration timeout, Fn fn) {
    return detail::run_scoped([fn = std::move(fn)](CancelScope&) mutable { return fn(); },
                              detail::ScopeSpec{.use_timeout = true, .timeout = timeout});
}

/// Run `fn` in a scope with an absolute deadline on the loop clock.
///
/// Same contract as with_timeout, with the deadline expressed absolutely.
template<typename Fn>
[[nodiscard]] auto with_deadline(TimePoint deadline, Fn fn) {
    return detail::run_scoped([fn = std::move(fn)](CancelScope&) mutable { return fn(); },
                              detail::ScopeSpec{.use_deadline = true, .deadline = deadline});
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

/// The innermost cancel scope of the awaiting coroutine.
///
/// Returns immediately without suspending, and borrows: the scope is owned by
/// the combinator or group that created it and outlives this await. Reading it
/// is not a cancellation checkpoint — only `cancel()` and `throw_if_cancelled()`
/// change what the scope does, and both are explicit calls.
/// Precondition: awaited inside coro::run, where every frame has a scope.
[[nodiscard]] inline auto current_scope() noexcept {
    return detail::CurrentScope{};
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_SCOPE_HPP