// Coroutine runtime — the awaitable behind AsyncMutex::lock().
//
// Depends on leaf headers only (the guard and the shared state), never on
// async_mutex.hpp: awaiting a member function requires that member's awaitable
// to be complete at the call site, so async_mutex.hpp includes *this* header
// and this header must not include that one. See detail/mutex_state.h.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_WAIT_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_WAIT_H

#include <coroutine>

#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/mutex_state.h"
#include "coro/mutex_guard.hpp"

namespace coro::detail {

/// Awaits one AsyncMutex, yielding its public RAII guard.
class AsyncMutexLockAwaitable {
public:
    explicit AsyncMutexLockAwaitable(MutexState& state) noexcept : state_(&state) {}

    AsyncMutexLockAwaitable(const AsyncMutexLockAwaitable&) = delete;
    AsyncMutexLockAwaitable& operator=(const AsyncMutexLockAwaitable&) = delete;

    constexpr bool await_ready() const noexcept { return false; }

    /// Acquires immediately when free, otherwise queues in FIFO order.
    /// Allocates (queue growth), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        scope_ = promise.scope;
        if (scope_ != nullptr && scope_->cancelled()) {
            cancelled_ = true;
            return false;
        }
        if (state_->try_acquire()) {
            return false;
        }
        node_.waiter = &promise;
        node_.cancelled_flag = &cancelled_;
        node_.owner = this;
        node_.on_cancel = &AsyncMutexLockAwaitable::on_cancel;
        state_->enqueue(&node_);
        if (scope_ != nullptr) {
            ScopeAccess::add_waiter(*scope_, node_);
        }
        return true;
    }

    /// Cancellation throws after releasing any lock already handed off.
    [[nodiscard]] MutexGuard await_resume() {
        if (node_.linked && node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*node_.scope, node_);
        }
        MutexGuard guard = cancelled_ ? MutexGuard{} : MutexGuard{*state_};
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        return guard;
    }

private:
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<AsyncMutexLockAwaitable*>(node.owner);
        self->state_->drop(node);
    }

    MutexState* state_ = nullptr;
    CancelScope* scope_ = nullptr;
    WaitNode node_{};
    bool cancelled_ = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_MUTEX_WAIT_H