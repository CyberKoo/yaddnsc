// Coroutine runtime — the awaitable behind AsyncMutex::lock().
//
// Depends on leaf headers only (the guard and the shared state), never on
// async_mutex.hpp: awaiting a member function requires that member's awaitable
// to be complete at the call site, so async_mutex.hpp includes *this* header
// and this header must not include that one. See detail/mutex_state.h.
#ifndef YADDNSC_CORO_DETAIL_MUTEX_WAIT_H
#define YADDNSC_CORO_DETAIL_MUTEX_WAIT_H

#include <coroutine>

#include "coro/detail/access.h"
#include "coro/detail/mutex_state.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/wait_operation.h"
#include "coro/mutex_guard.hpp"

namespace coro::detail {

/// Awaits one AsyncMutex, yielding its public RAII guard.
///
/// The hand-off is the delicate part: the lock is transferred by the queue, not
/// by the resume, so cancellation after the hand-off still has to produce a
/// guard. The queue therefore sets `granted` before it reports the completion,
/// and await_resume takes the lock before it looks at cancellation.
class AsyncMutexLockAwaiter {
public:
    explicit AsyncMutexLockAwaiter(MutexState& state) noexcept : state_(&state) {}

    AsyncMutexLockAwaiter(const AsyncMutexLockAwaiter&) = delete;
    AsyncMutexLockAwaiter& operator=(const AsyncMutexLockAwaiter&) = delete;
    AsyncMutexLockAwaiter(AsyncMutexLockAwaiter&&) = delete;
    AsyncMutexLockAwaiter& operator=(AsyncMutexLockAwaiter&&) = delete;

    constexpr bool await_ready() const noexcept { return false; }

    /// Acquires immediately when free, otherwise queues in FIFO order.
    /// Allocates (queue growth), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        scope_ = promise.exec->scope();
        if (scope_ != nullptr && scope_->cancelled()) {
            cancelled_ = true;
            return false;
        }
        if (state_->try_acquire()) {
            return false;
        }
        queued_.operation = &wait_;
        wait_.begin(*promise.exec);
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &AsyncMutexLockAwaiter::on_cancel);
        }
        try {
            state_->enqueue(&queued_);
        } catch (...) {
            // The queue never took the waiter, so only the wait binding is left.
            wait_.rollback();
            throw;
        }
        wait_.commit_suspend();
        return true;
    }

    /// Takes the lock if it was handed over, then reports cancellation: a lock
    /// that already transferred to this waiter must be given to the guard, which
    /// returns it while the cancellation unwinds.
    [[nodiscard]] MutexGuard await_resume() {
        wait_.begin_resume();
        wait_.finish();
        const bool holds_lock = queued_.granted || !cancelled_;
        MutexGuard guard = holds_lock ? MutexGuard{*state_} : MutexGuard{};
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        return guard;
    }

private:
    /// Leave the queue before hand-off, or retain the granted lock afterwards.
    /// Cancellation still runs after queueing; await_resume returns that lock.
    static CancelAction on_cancel(void* context) noexcept {
        auto* self = static_cast<AsyncMutexLockAwaiter*>(context);
        self->state_->drop(self->queued_);
        self->cancelled_ = true;
        return CancelAction::RESUME;
    }

    MutexState* state_ = nullptr;
    CancelScope* scope_ = nullptr;
    MutexWaiter queued_{};
    CancelSubscription subscription_{};
    WaitOperation wait_{subscription_};
    bool cancelled_ = false;
};

/// Reusable lock operation; each await owns an independent registration.
class AsyncMutexLockAwaitable {
public:
    explicit AsyncMutexLockAwaitable(MutexState& state) noexcept : state_(&state) {}

    [[nodiscard]] AsyncMutexLockAwaiter operator co_await() const noexcept { return AsyncMutexLockAwaiter{*state_}; }

private:
    MutexState* state_;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_MUTEX_WAIT_H
