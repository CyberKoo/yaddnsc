// Coroutine runtime — internal notification state.
//
// Kept as a leaf detail header so the waiter (detail/notification_wait.h) never
// includes the public notification.hpp: the same acyclic-include rule as
// detail/mutex_state.h.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_STATE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_STATE_H

namespace coro::detail {

class WaitOperation;

/// Shared state between a Notification and its one live wait.
struct NotificationState {
    /// A delivered notification not yet consumed by a wait.
    bool pending = false;
    /// The live wait's operation, from its await_suspend until its await_resume.
    /// A queued-but-unresumed wait is still live: not resumed is not finished.
    WaitOperation* waiter = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NOTIFICATION_STATE_H
