// Coroutine runtime — the timer awaitable behind sleep_for / sleep_until.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SLEEP_AWAITABLE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SLEEP_AWAITABLE_H

#include <stdexcept>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/detail/timer_node.h"
#include "infrastructure/coro/detail/wait_node.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/time.h"

namespace coro::detail {

/// Awaits a point in time on the loop clock.
///
/// Ownership: the timer node and the scope waiter are members, so they live in
/// the awaiting frame and die with it; the loop and the scope hold pointers to
/// them only while the frame is parked.
/// Cancellation: a checkpoint — a cancelled scope resumes the frame early.
/// Thread safety: loop thread only.
class SleepAwaitable {
public:
    /// Suspend for `delay` relative to the loop clock.
    static SleepAwaitable after(Duration delay) noexcept { return SleepAwaitable{delay, TimePoint{}, true}; }

    /// Suspend until an absolute loop-clock time.
    static SleepAwaitable at(TimePoint deadline) noexcept { return SleepAwaitable{Duration{}, deadline, false}; }

    SleepAwaitable(const SleepAwaitable&) = delete;
    SleepAwaitable& operator=(const SleepAwaitable&) = delete;
    SleepAwaitable(SleepAwaitable&&) = delete;
    SleepAwaitable& operator=(SleepAwaitable&&) = delete;
    ~SleepAwaitable() = default;

    constexpr bool await_ready() const noexcept { return false; }

    /// Arms the timer and the scope waiter unless the scope is already
    /// cancelled or the deadline has passed. Allocates (timer-heap growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr) {
            throw std::logic_error("sleep requires a running loop");
        }
        const TimePoint deadline = relative_ ? loop_->now() + delay_ : deadline_;
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        if (deadline <= loop_->now()) {
            LoopAccess::schedule(*loop_, promise);
            return true;  // due sleeps still yield to other tasks
        }
        arm(*this, promise, deadline);
        return true;
    }

    /// Returns nothing on a completed sleep, `coro::Cancelled` when the
    /// scope cancelled the wait. Cancellation throws `Cancelled`.
    void await_resume() {
        if (node_.linked && node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*node_.scope, node_);
        }
        if (armed_) {
            LoopAccess::remove_timer(*loop_, timer_);
            armed_ = false;
        }
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
    }

private:
    SleepAwaitable(Duration delay, TimePoint deadline, bool relative) noexcept
        : delay_(delay), deadline_(deadline), relative_(relative) {}

    static void arm(SleepAwaitable& self, PromiseBase& promise, TimePoint deadline) {
        self.frame_ = &promise;
        LoopAccess::add_timer(*self.loop_, self.timer_, deadline, &SleepAwaitable::on_timer, &self);
        self.armed_ = true;
        self.node_.waiter = &promise;
        self.node_.owner = &self;
        self.node_.on_cancel = &SleepAwaitable::on_cancel;
        if (self.scope_ != nullptr) {
            ScopeAccess::add_waiter(*self.scope_, self.node_);
        }
    }

    /// Timer action: the loop has already popped the timer and runs this on the
    /// loop thread. Never throws.
    static void on_timer(void* context) noexcept {
        auto* self = static_cast<SleepAwaitable*>(context);
        self->armed_ = false;
        if (self->node_.linked && self->node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*self->node_.scope, self->node_);
        }
        if (self->frame_ != nullptr) {
            wake(*self->frame_);
        }
    }

    /// Cancellation hook: drop the timer so the deadline cannot fire later.
    /// Never throws; runs on the loop thread.
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<SleepAwaitable*>(node.owner);
        if (self->armed_) {
            LoopAccess::remove_timer(*self->loop_, self->timer_);
            self->armed_ = false;
        }
    }

    Duration delay_{};
    TimePoint deadline_{};
    bool relative_ = true;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    PromiseBase* frame_ = nullptr;
    TimerNode timer_{};
    WaitNode node_{};
    bool armed_ = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SLEEP_AWAITABLE_H