//
// Coroutine runtime — Notification.
//
// A Notification hands a state change from one coroutine to one waiting
// coroutine. The producer updates shared state and then calls notify(); the
// consumer co_awaits wait() and re-checks its condition after every wake, so
// the notification itself carries neither a result nor a count:
//
//   changed.notify();
//   co_await changed.wait();
//
// Contract:
// - notify() with no live wait retains one pending notification.
// - Repeated notifications coalesce into one; nothing is counted.
// - notify() with a live wait enqueues the waiter on the ready queue; the
//   frame never runs on the notifier's stack.
// - Cancellation of the waiting scope wakes the waiter, and await_resume
//   throws coro::Cancelled.
// - Cancellation that lands after notify() but before the wake is resumed
//   wins: the wait throws and the notification stays pending for a later
//   wait.
// - A wait that completes normally consumes the pending notification.
// - A second live wait is rejected with std::logic_error; the original wait
//   stays valid.
//
// notify(), construction and destruction never allocate and never throw. The
// waiter's pointer outlives its scope registration: a notified wait stays
// "live" until its await_resume has cleaned up, so destroying a Notification
// with a queued-but-unresumed waiter is as much a lifetime violation as
// destroying one with a parked waiter.
//
// Thread safety: loop thread only, like the scope tree waits register on.
// Foreign threads communicate through Loop::post, offload completion and
// atomics, never by touching a Notification.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_NOTIFICATION_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_NOTIFICATION_HPP

#include <cassert>
#include <exception>

#include "coro/detail/access.h"
#include "coro/detail/notification_state.h"
#include "coro/detail/notification_wait.h"  // IWYU pragma: export
#include "coro/detail/wait_node.h"

namespace coro {

/// A single-waiter, coalescing state-change notification. See the header
/// comment for the delivery contract.
///
/// Lifetime: a Notification must outlive every wait on it. Destruction with a
/// live wait is a contract violation: it terminates in production, with an
/// assertion for Debug diagnosis — the same policy as CancelScope.
class Notification {
public:
    Notification() = default;

    ~Notification() noexcept {
        assert(state_.waiter == nullptr && "notification destroyed with a live wait");
        if (state_.waiter != nullptr) {
            std::terminate();  // the waiter would resume against a dead object
        }
    }

    Notification(const Notification&) = delete;
    Notification& operator=(const Notification&) = delete;
    Notification(Notification&&) = delete;
    Notification& operator=(Notification&&) = delete;

    /// Deliver a notification: wake the live waiter through the ready queue,
    /// or retain one pending notification when nobody waits. Never allocates.
    void notify() noexcept {
        state_.pending = true;
        detail::WaitNode* waiter = state_.waiter;
        if (waiter != nullptr && !waiter->scheduled) {
            waiter->scheduled = true;
            detail::wake(*waiter->waiter);
        }
    }

    /// Wait for the next notification. The returned operation borrows the
    /// notification; each co_await registers an independent waiter.
    /// Cancellation throws coro::Cancelled and keeps the notification pending.
    [[nodiscard]] detail::NotificationWaitable wait() noexcept { return detail::NotificationWaitable{state_}; }

private:
    detail::NotificationState state_;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_NOTIFICATION_HPP
