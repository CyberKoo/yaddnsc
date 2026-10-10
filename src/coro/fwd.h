// Coroutine runtime — public forward declarations.
//
// Public class and class-template declarations, so headers can name runtime
// types without pulling in their definitions. Type aliases live in their owning
// headers.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_FWD_H
#define YADDNSC_INFRASTRUCTURE_CORO_FWD_H

namespace coro {

class AsyncMutex;
class CancelScope;
class Clock;
class Loop;
class ManualClock;
class MutexGuard;
class SystemClock;
class TaskGroup;

template<typename T>
class Handle;
template<typename T>
class Task;
template<typename T>
struct ScopeOutcome;

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_FWD_H