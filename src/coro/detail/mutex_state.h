// Coroutine runtime — the state an AsyncMutex and its guard share.
//
// Leaf implementation type: it depends on other detail headers only, which is
// what lets detail/mutex_wait.h build the lock awaiter without async_mutex.hpp
// and keeps the module's include graph acyclic.
#ifndef YADDNSC_CORO_DETAIL_MUTEX_STATE_H
#define YADDNSC_CORO_DETAIL_MUTEX_STATE_H

#include <deque>

#include "coro/detail/wait_operation.h"

namespace coro::detail {

/// One queued lock request.
///
/// The queue owns nothing: it points at the awaiting frame's wait operation and
/// carries the hand-off flag. `granted` is set before the completion is
/// reported, so a cancellation that lands between the hand-off and the resume
/// still finds a lock to take.
struct MutexWaiter {
    /// The operation that will resume this waiter.
    WaitOperation* operation = nullptr;
    /// True once the lock was handed to this waiter.
    bool granted = false;
};

/// The lock flag and the FIFO waiter queue of one AsyncMutex.
///
/// Split out from AsyncMutex so that both the RAII guard and the lock awaitable
/// can be defined without the mutex class being complete. Awaiting a member
/// function requires that member's awaitable to be complete at every call site,
/// which in turn requires async_mutex.hpp to include the awaitable's header — so
/// the awaitable may not reach back for AsyncMutex, and therefore may not name
/// anything nested inside it.
struct MutexState {
    /// Take the lock if free. Returns false when a waiter is already queued,
    /// which is what keeps the hand-off FIFO.
    bool try_acquire() noexcept {
        if (locked) {
            return false;
        }
        locked = true;
        return true;
    }

    void enqueue(MutexWaiter* waiter) { queue.push_back(waiter); }

    /// Hand the lock to the next live waiter (FIFO); release it if none.
    /// Never throws: it only walks the queue and reports the completion. The
    /// hand-off flag is set first, so a cancellation racing the resume cannot
    /// make the waiter believe it never got the lock.
    void release() noexcept {
        while (!queue.empty()) {
            MutexWaiter* waiter = queue.front();
            queue.pop_front();
            if (waiter->operation->resume_pending()) {
                continue;  // cancelled before the hand-off
            }
            waiter->granted = true;
            waiter->operation->event_ready();
            return;  // lock stays held, transferred to that waiter
        }
        locked = false;
    }

    /// Remove a waiter whose scope was cancelled before it was granted.
    void drop(MutexWaiter& waiter) noexcept {
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (*it == &waiter) {
                queue.erase(it);
                return;
            }
        }
    }

    bool locked = false;
    std::deque<MutexWaiter*> queue;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_MUTEX_STATE_H