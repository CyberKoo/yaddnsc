// Coroutine runtime — public time values, without a clock object.
#ifndef YADDNSC_CORO_TIME_H
#define YADDNSC_CORO_TIME_H

#include <chrono>

namespace coro {
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;
}  // namespace coro

#endif  // YADDNSC_CORO_TIME_H
