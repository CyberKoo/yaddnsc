// Coroutine runtime — the timer awaitable behind sleep_for / sleep_until.
#ifndef YADDNSC_CORO_DETAIL_SLEEP_AWAITABLE_H
#define YADDNSC_CORO_DETAIL_SLEEP_AWAITABLE_H

#include <cassert>
#include <coroutine>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/time_math.h"
#include "coro/detail/timer_node.h"
#include "coro/detail/wait_operation.h"
#include "coro/loop.h"
#include "coro/time.h"

namespace coro::detail {

/// Awaits a point in time on the loop clock.
///
/// Precondition: awaited inside coro::run (the frame must run on a loop).
/// Ownership: the timer node and the wait operation are members, so they live in
/// the awaiting frame and die with it; the loop and the scope hold pointers to
/// them only while the frame is parked.
/// Cancellation: a checkpoint — a cancelled scope resumes the frame early.
/// Thread safety: loop thread only.
class SleepAwaiter {
public:
    SleepAwaiter(const SleepAwaiter&) = delete;
    SleepAwaiter& operator=(const SleepAwaiter&) = delete;
    SleepAwaiter(SleepAwaiter&&) = delete;
    SleepAwaiter& operator=(SleepAwaiter&&) = delete;
    // Started frames drain before destruction; firing/cancellation removes
    // registration before resume. Destroying a parked frame is not supported.
    ~SleepAwaiter() = default;

    constexpr bool await_ready() const noexcept { return false; }

    /// Arms the timer and the scope waiter unless the scope is already
    /// cancelled or the deadline has passed. Allocates (timer-heap growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        assert(promise.exec != nullptr && "sleep must be awaited inside coro::run");
        loop_ = promise.exec->loop();
        scope_ = promise.exec->scope();
        assert(loop_ != nullptr && "sleep_for()/sleep_until() must be awaited inside coro::run");
        const TimePoint deadline = relative_ ? saturated_deadline(loop_->now(), delay_) : deadline_;
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        // A due sleep still yields to the loop: it queues the resume and parks,
        // rather than continuing inline.
        wait_.begin(*promise.exec);
        if (deadline <= loop_->now()) {
            wait_.event_ready();
            wait_.commit_suspend();
            return true;
        }
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &SleepAwaiter::on_cancel);
        }
        try {
            LoopAccess::add_timer(*loop_, timer_, deadline, &SleepAwaiter::on_timer, this);
        } catch (...) {
            // The timer was not linked, so only the wait binding is left to undo.
            wait_.rollback();
            throw;
        }
        armed_ = true;
        wait_.commit_suspend();
        return true;
    }

    /// Returns nothing on a completed sleep, `coro::Cancelled` when the
    /// scope cancelled the wait. Cancellation throws `Cancelled`.
    void await_resume() {
        wait_.begin_resume();
        wait_.finish();
        if (armed_) {
            LoopAccess::remove_timer(*loop_, timer_);
            armed_ = false;
        }
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
    }

public:
    SleepAwaiter(Duration delay, TimePoint deadline, bool relative) noexcept
        : delay_(delay), deadline_(deadline), relative_(relative) {}

private:
    static void disarm(SleepAwaiter& self) noexcept {
        if (self.armed_) {
            LoopAccess::remove_timer(*self.loop_, self.timer_);
            self.armed_ = false;
        }
    }

    /// Timer action: the loop has already popped the timer and runs this on the
    /// loop thread. Never throws.
    static void on_timer(void* context) noexcept {
        auto* self = static_cast<SleepAwaiter*>(context);
        self->armed_ = false;
        // The only completion entry: the operation decides whether this becomes
        // the resume, or whether a cancellation already took it.
        self->wait_.event_ready();
    }

    /// Cancellation hook: drop the timer so the deadline cannot fire later, and
    /// take the resume. Never throws; runs on the loop thread.
    static detail::CancelAction on_cancel(void* context) noexcept {
        auto* self = static_cast<SleepAwaiter*>(context);
        disarm(*self);
        return detail::CancelAction::RESUME;
    }

    Duration delay_{};
    TimePoint deadline_{};
    bool relative_ = true;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    TimerNode timer_{};
    CancelSubscription subscription_{};
    detail::WaitOperation wait_{subscription_};
    bool armed_ = false;
};

/// Reusable timer operation; relative deadlines are sampled at each await.
class SleepAwaitable {
public:
    static SleepAwaitable after(Duration delay) noexcept { return SleepAwaitable{delay, TimePoint{}, true}; }

    static SleepAwaitable at(TimePoint deadline) noexcept { return SleepAwaitable{Duration{}, deadline, false}; }

    [[nodiscard]] SleepAwaiter operator co_await() const noexcept { return SleepAwaiter{delay_, deadline_, relative_}; }

private:
    SleepAwaitable(Duration delay, TimePoint deadline, bool relative) noexcept
        : delay_(delay), deadline_(deadline), relative_(relative) {}

    Duration delay_;
    TimePoint deadline_;
    bool relative_;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_SLEEP_AWAITABLE_H
