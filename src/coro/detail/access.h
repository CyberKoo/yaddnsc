// Coroutine runtime — the runtime's only door into Loop and CancelScope.
//
// Both classes keep their registration and bookkeeping operations private: a
// caller that reached them directly could bypass the ready queue (axiom 4) or
// leave a waiter linked into a scope it does not own. Every runtime awaitable
// goes through the two access structs declared here instead.
#ifndef YADDNSC_CORO_DETAIL_ACCESS_H
#define YADDNSC_CORO_DETAIL_ACCESS_H

#include <cassert>
#include <functional>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/fd_entry.h"
#include "coro/detail/inbox_node.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/timer_node.h"
#include "coro/detail/wait_operation.h"
#include "coro/loop.h"

namespace coro::detail {

/// Runtime-only access to the loop's scheduling and registration services.
///
/// Schedule goes through the ready queue, so a resumed frame never runs on a
/// caller's stack; the registration calls are the loop side of a timer, fd or
/// signal awaitable and always pair with a matching removal.
struct LoopAccess {
    static void schedule(Loop& loop, ExecutionRecord& record) noexcept { loop.schedule(record); }

    /// Terminal entry: the frame reached final_suspend. Marks the record and
    /// queues it for retirement when it has no execution children left.
    static void mark_terminal(Loop& loop, ExecutionRecord& record) noexcept { loop.mark_terminal(record); }

    static void register_execution(Loop& loop, ExecutionRecord& record) noexcept { loop.register_execution(record); }

    static void add_timer(Loop& loop, TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept,
                          void* context) {
        loop.add_timer(timer, deadline, action, context);
    }

    static void remove_timer(Loop& loop, TimerNode& timer) noexcept { loop.remove_timer(timer); }

    static void add_fd(Loop& loop, FdEntry& entry) { loop.add_fd(entry); }

    static void remove_fd(Loop& loop, FdEntry& entry) noexcept { loop.remove_fd(entry); }

    [[nodiscard]] static int poll_error(const Loop& loop) noexcept { return loop.poll_error(); }

    static void admit_offload(Loop& loop, OffloadAdmission& admission) { loop.admit_offload(admission); }

    static void cancel_offload(Loop& loop, OffloadAdmission& admission) noexcept { loop.cancel_offload(admission); }

    static void post_node(Loop& loop, InboxNode& node) noexcept { loop.post_node(node); }
};

/// Runtime-only access to a scope's waiter bookkeeping and cancellation cause.
///
/// Application code cancels a scope and observes it. Registering a parked
/// waiter, choosing a cause and deciding what a scope absorbs stay internal:
/// each is a bookkeeping invariant of the node that owns the waiter.
struct ScopeAccess {
    static void cancel(CancelScope& scope, CancelCause cause) noexcept { scope.cancel(cause); }

    static void add_subscriber(CancelScope& scope, CancelSubscription& subscription) noexcept {
        scope.add_subscriber(subscription);
    }

    [[nodiscard]] static bool absorbs(const CancelScope& scope, const Cancelled& error) noexcept {
        return scope.absorbs(error);
    }

    /// Timer action for the timeout and deadline combinators.
    static void timeout_action(void* context) noexcept {
        static_cast<CancelScope*>(context)->cancel(CancelCause::TIMEOUT);
    }
};

/// Resumes a parked frame through the loop's ready queue, addressed by the
/// record that owns it. Every started frame has an execution record bound by its
/// activation, so the loop and the cancel scope both come from the record
/// itself. A frame that was never started has no record and nothing to wake.
inline void wake(PromiseBase& frame) noexcept {
    assert(frame.exec != nullptr && "waking a frame that was never started");
    if (frame.exec == nullptr) {
        return;
    }
    LoopAccess::schedule(*frame.exec->loop(), *frame.exec);
}

/// Resumes a record directly, for callers that already hold one.
inline void wake(ExecutionRecord& record) noexcept {
    LoopAccess::schedule(*record.loop(), record);
}

/// Defined in detail/access.h rather than next to the wait protocol, so that the
/// protocol header stays independent of Loop.
inline void schedule_resume(ExecutionRecord& record) noexcept {
    wake(record);
}

/// Defined here for the same reason as schedule_resume: the wait protocol header
/// stays independent of CancelScope's subscription bookkeeping.
inline void link_subscription(CancelScope& scope, CancelSubscription& subscription) noexcept {
    ScopeAccess::add_subscriber(scope, subscription);
}

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_ACCESS_H