// Coroutine runtime — the awaitable behind Notification::wait().
//
// Depends on leaf headers only (the shared state), never on notification.hpp:
// awaiting a member function requires that member's awaitable to be complete at
// the call site, so notification.hpp includes *this* header and this header
// must not include that one. See detail/mutex_wait.h for the same rule.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_WAIT_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_WAIT_H

#include <coroutine>
#include <stdexcept>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/notification_state.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/wait_operation.h"

namespace coro::detail {

/// Awaits one notification round on a Notification.
///
/// Each co_await owns an independent wait operation, so consecutive waits on the
/// same notification never inherit a previous round's registration or wake state.
class NotificationWaiter {
public:
    explicit NotificationWaiter(NotificationState& state) noexcept : state_(&state) {}

    NotificationWaiter(const NotificationWaiter&) = delete;
    NotificationWaiter& operator=(const NotificationWaiter&) = delete;
    NotificationWaiter(NotificationWaiter&&) = delete;
    NotificationWaiter& operator=(NotificationWaiter&&) = delete;

    /// A second live wait is a contract violation; the existing wait is
    /// left untouched.
    bool await_ready() const {
        if (state_->waiter != nullptr) {
            throw std::logic_error("coro::Notification supports a single live wait");
        }
        return false;
    }

    /// Parks on the current scope, unless the round completes at once. An
    /// already-cancelled scope wins over a retained notification.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        scope_ = promise.exec->scope();
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        if (state_->pending) {
            state_->pending = false;
            return false;
        }
        wait_.begin(*promise.exec);
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &NotificationWaiter::on_cancel);
        }
        state_->waiter = &wait_;
        wait_.commit_suspend();
        return true;
    }

    /// Ends the registration first, then lets cancellation beat the
    /// notification — a cancelled wait leaves the pending notification for the
    /// next round — and only a completed round consumes it.
    void await_resume() {
        wait_.begin_resume();
        wait_.finish();
        state_->waiter = nullptr;
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        state_->pending = false;
    }

private:
    /// Cancellation hook: leave the notification pending for the next round and
    /// take the resume.
    static CancelAction on_cancel(void* context) noexcept {
        static_cast<void>(context);
        return CancelAction::RESUME;
    }

    NotificationState* state_;
    CancelScope* scope_ = nullptr;
    CancelSubscription subscription_{};
    WaitOperation wait_{subscription_};
};

/// Reusable wait operation; each await owns an independent registration.
class NotificationWaitable {
public:
    explicit NotificationWaitable(NotificationState& state) noexcept : state_(&state) {}

    [[nodiscard]] NotificationWaiter operator co_await() const noexcept { return NotificationWaiter{*state_}; }

private:
    NotificationState* state_;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_WAIT_H
