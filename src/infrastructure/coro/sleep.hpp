//
// Coroutine runtime — cancellable sleeps.
//
// sleep_for / sleep_until park the awaiting frame on the loop's timer heap and
// register the same frame as a waiter on its innermost cancel scope. A scope
// cancellation therefore returns before the deadline, and the await surfaces the
// cancellation as `unexpected(operation_canceled)` — the value channel.
//

#ifndef YADDNSC_CORO_SLEEP_HPP
#define YADDNSC_CORO_SLEEP_HPP

#include <system_error>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro {

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

    bool await_ready() const noexcept { return false; }

    /// Arms the timer and the scope waiter unless the scope is already
    /// cancelled or the deadline has passed. Allocates (timer-heap growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr) {
            cancelled_ = true;  // awaited outside coro::run
            return false;
        }
        const TimePoint deadline = relative_ ? loop_->now() + delay_ : deadline_;
        if (scope_ != nullptr && scope_->cancelled()) {
            cancelled_ = true;
            return false;
        }
        if (deadline <= loop_->now()) {
            return false;  // already due: a checkpoint, but no suspension
        }
        arm(*this, promise, deadline);
        return true;
    }

    /// Returns nothing on a completed sleep, `operation_canceled` when the
    /// scope cancelled the wait. Never throws.
    [[nodiscard]] std::expected<void, std::errc> await_resume() noexcept {
        if (node_.linked && node_.scope != nullptr) {
            node_.scope->remove_waiter(node_);
        }
        if (armed_) {
            loop_->remove_timer(timer_);
            armed_ = false;
        }
        if (cancelled_) {
            return std::unexpected(std::errc::operation_canceled);
        }
        return {};
    }

private:
    SleepAwaitable(Duration delay, TimePoint deadline, bool relative) noexcept
        : delay_(delay), deadline_(deadline), relative_(relative) {}

    static void arm(SleepAwaitable& self, PromiseBase& promise, TimePoint deadline) {
        self.frame_ = &promise;
        self.timer_.action = &SleepAwaitable::on_timer;
        self.timer_.context = &self;
        self.loop_->add_timer(self.timer_, deadline, &SleepAwaitable::on_timer, &self);
        self.armed_ = true;
        self.node_.waiter = &promise;
        self.node_.cancelled_flag = &self.cancelled_;
        self.node_.owner = &self;
        self.node_.on_cancel = &SleepAwaitable::on_cancel;
        if (self.scope_ != nullptr) {
            self.scope_->add_waiter(self.node_);
        }
    }

    /// Timer action: the loop has already popped the timer and runs this on the
    /// loop thread. Never throws.
    static void on_timer(void* context) noexcept {
        auto* self = static_cast<SleepAwaitable*>(context);
        self->armed_ = false;
        if (self->node_.linked && self->node_.scope != nullptr) {
            self->node_.scope->remove_waiter(self->node_);
        }
        if (self->frame_ != nullptr && self->frame_->loop != nullptr) {
            self->frame_->loop->schedule(*self->frame_);
        }
    }

    /// Cancellation hook: drop the timer so the deadline cannot fire later.
    /// Never throws; runs on the loop thread.
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<SleepAwaitable*>(node.owner);
        if (self->armed_) {
            self->loop_->remove_timer(self->timer_);
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
    bool cancelled_ = false;
    bool armed_ = false;
};

/// Suspend for `delay` on the loop clock. Cancellable at the await; ignoring
/// the returned cancellation flag is a supported usage (see the scheduling loop
/// in the design), so the result is deliberately not [[nodiscard]].
inline SleepAwaitable sleep_for(Duration delay) noexcept {
    return SleepAwaitable::after(delay);
}

/// Suspend until `deadline` on the loop clock. Cancellable at the await.
inline SleepAwaitable sleep_until(TimePoint deadline) noexcept {
    return SleepAwaitable::at(deadline);
}

}  // namespace coro

#endif  // YADDNSC_CORO_SLEEP_HPP
