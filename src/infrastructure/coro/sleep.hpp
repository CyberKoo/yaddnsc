//
// Coroutine runtime — cancellable sleeps.
//
// sleep_for / sleep_until park the awaiting frame on the loop's timer heap and
// register the same frame as a waiter on its innermost cancel scope. A scope
// cancellation therefore returns before the deadline, and the await surfaces the
// cancellation as `coro::Cancelled` — the control exception channel.
//
// The awaitable itself is detail: the return type is deduced, so a caller never
// names it.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_SLEEP_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_SLEEP_HPP

#include "infrastructure/coro/detail/sleep_awaitable.h"
#include "infrastructure/coro/time.h"

namespace coro {

/// Suspend for `delay` on the loop clock. Cancellable at the await: a
/// cancelled scope throws `Cancelled` immediately. Even a zero-delay sleep
/// yields to the loop when the scope is active.
[[nodiscard]] inline auto sleep_for(Duration delay) noexcept {
    return detail::SleepAwaitable::after(delay);
}

/// Suspend until `deadline` on the loop clock. Cancellable at the await.
[[nodiscard]] inline auto sleep_until(TimePoint deadline) noexcept {
    return detail::SleepAwaitable::at(deadline);
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_SLEEP_HPP