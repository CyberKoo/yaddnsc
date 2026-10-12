// Coroutine runtime — saturating arithmetic for public time values.
#ifndef YADDNSC_CORO_DETAIL_TIME_MATH_H
#define YADDNSC_CORO_DETAIL_TIME_MATH_H

#include "coro/time.h"

namespace coro::detail {

/// Clamp an unrepresentable deadline instead of overflowing the clock's rep.
[[nodiscard]] constexpr TimePoint saturated_deadline(TimePoint now, Duration delay) noexcept {
    const Duration elapsed = now.time_since_epoch();
    if (delay > Duration::zero() && elapsed > Duration::max() - delay) {
        return TimePoint::max();
    }
    if (delay < Duration::zero() && elapsed < Duration::min() - delay) {
        return TimePoint::min();
    }
    return TimePoint{elapsed + delay};
}

/// Clamp a distance that cannot be represented by a Duration.
[[nodiscard]] constexpr Duration saturated_difference(TimePoint end, TimePoint start) noexcept {
    const Duration lhs = end.time_since_epoch();
    const Duration rhs = start.time_since_epoch();
    if (rhs > Duration::zero() && lhs < Duration::min() + rhs) {
        return Duration::min();
    }
    if (rhs < Duration::zero() && lhs > Duration::max() + rhs) {
        return Duration::max();
    }
    return lhs - rhs;
}

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_TIME_MATH_H
