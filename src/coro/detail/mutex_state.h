// Coroutine runtime — the state an AsyncMutex and its guard share.
//
// Leaf implementation type: it depends on other detail headers only, which is
// what lets detail/mutex_wait.h build the lock awaiter without async_mutex.hpp
// and keeps the module's include graph acyclic.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_STATE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_STATE_H

#include <deque>

#include "coro/detail/access.h"
#include "coro/detail/wait_node.h"

namespace coro::detail {

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

    void enqueue(WaitNode* node) { queue.push_back(node); }

    /// Hand the lock to the next live waiter (FIFO); release it if none.
    /// Never throws: it only walks the queue and schedules.
    void release() noexcept {
        while (!queue.empty()) {
            WaitNode* node = queue.front();
            queue.pop_front();
            if (node->scheduled) {
                continue;  // cancelled before the hand-off
            }
            node->scheduled = true;
            if (node->linked && node->scope != nullptr) {
                ScopeAccess::remove_waiter(*node->scope, *node);
            }
            if (node->waiter != nullptr) {
                wake(*node->waiter);
            }
            return;  // lock stays held, transferred to that waiter
        }
        locked = false;
    }

    /// Remove a waiter whose scope was cancelled before it was granted.
    void drop(WaitNode& node) noexcept {
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (*it == &node) {
                queue.erase(it);
                return;
            }
        }
    }

    bool locked = false;
    std::deque<WaitNode*> queue;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_STATE_H