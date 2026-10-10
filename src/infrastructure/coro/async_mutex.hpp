//
// Coroutine runtime — AsyncMutex.
//
// Persistent resources (HTTP sessions, DoH/DoT connections) are shared by
// awaiting, not by rejecting: `co_await mutex.lock()` queues a waiter in FIFO
// order and yields a move-only MutexGuard that releases on destruction. A
// cancelled waiter leaves the queue and observes `coro::Cancelled`.
//
// The mutex is a thin owner of detail::MutexState. The lock awaitable lives in
// detail/mutex_wait.h and deliberately cannot name AsyncMutex — awaiting a
// member requires its awaitable to be complete at the call site, so this header
// includes that one and the dependency may not run back. See
// detail/mutex_state.h.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_ASYNC_MUTEX_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_ASYNC_MUTEX_HPP

#include "infrastructure/coro/detail/mutex_state.h"
#include "infrastructure/coro/detail/mutex_wait.h"  // IWYU pragma: export
#include "infrastructure/coro/mutex_guard.hpp"

namespace coro {

/// An async, FIFO, non-rejecting mutex.
///
/// Thread safety: loop thread only. The mutex coordinates coroutines on one
/// loop; it is not a thread synchronization primitive.
/// Lifetime: a mutex must outlive every await and every Guard it hands out.
class AsyncMutex {
public:
    /// The RAII result of `co_await lock()`; see coro::MutexGuard.
    using Guard = MutexGuard;

    AsyncMutex() = default;
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    ~AsyncMutex() = default;

    /// Awaiting the returned object yields `Guard`; cancellation throws
    /// `coro::Cancelled` and removes the waiter without granting the lock.
    /// The mutex must outlive the await.
    [[nodiscard]] detail::AsyncMutexLockAwaitable lock() noexcept { return detail::AsyncMutexLockAwaitable{state_}; }

    /// True while the mutex is held by someone.
    [[nodiscard]] bool locked() const noexcept { return state_.locked; }

private:
    detail::MutexState state_;
};
}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_ASYNC_MUTEX_HPP
