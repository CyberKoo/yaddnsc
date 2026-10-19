// Coroutine runtime — public forward declarations.
//
// Public class and class-template declarations, so headers can name runtime
// types without pulling in their definitions. Enums and the result alias live
// here too, because a public vocabulary a runtime detail header must name
// cannot be declared in a header that header itself includes.
#ifndef YADDNSC_CORO_FWD_H
#define YADDNSC_CORO_FWD_H

#include <exception>
#include <expected>

namespace coro {

class AsyncMutex;
class CancelScope;
class Clock;
class Loop;
class ManualClock;
class MutexGuard;
class Notification;
class SystemClock;
class Nursery;

/// Child defect policy: stop siblings at first defect, or report unobserved errors at exit.
enum class NurseryPolicy { FAIL_FAST, SUPERVISOR };

/// A child's value or defect. Cancellation is not a Result error.
template<typename T>
using Result = std::expected<T, std::exception_ptr>;

template<typename T>
class Handle;
template<typename T>
class Task;
template<typename T>
struct ScopeOutcome;

}  // namespace coro

#endif  // YADDNSC_CORO_FWD_H