// Coroutine runtime — the runtime's only door into Loop and CancelScope.
//
// Both classes keep their registration and bookkeeping operations private: a
// caller that reached them directly could bypass the ready queue (axiom 4) or
// leave a waiter linked into a scope it does not own. Every runtime awaitable
// goes through the two access structs declared here instead.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_ACCESS_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_ACCESS_H

#include <functional>
#include <utility>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/detail/timer_node.h"
#include "infrastructure/coro/detail/wait_node.h"
#include "infrastructure/coro/loop.h"

namespace coro::detail {

/// Runtime-only access to the loop's scheduling and registration services.
///
/// Schedule goes through the ready queue, so a resumed frame never runs on a
/// caller's stack; the registration calls are the loop side of a timer, fd or
/// signal awaitable and always pair with a matching removal.
struct LoopAccess {
    static void schedule(Loop& loop, PromiseBase& frame) noexcept { loop.schedule(frame); }

    static void add_timer(Loop& loop, TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept,
                          void* context) {
        loop.add_timer(timer, deadline, action, context);
    }

    static void remove_timer(Loop& loop, TimerNode& timer) noexcept { loop.remove_timer(timer); }

    static FdToken add_fd(Loop& loop, int fd, short events, void (*fn)(void*, short) noexcept, void* context) {
        return loop.add_fd(fd, events, fn, context);
    }

    static void remove_fd(Loop& loop, FdToken token) noexcept { loop.remove_fd(token); }

    static void arm_signal(Loop& loop, int sig, WaitNode& node) { loop.arm_signal(sig, node); }

    static void disarm_signal(Loop& loop, int sig, WaitNode& node) noexcept { loop.disarm_signal(sig, node); }

    static void submit_offload(Loop& loop, std::function<void()> job) {
        loop.submit_offload(std::move(job));
    }
};

/// Runtime-only access to a scope's waiter bookkeeping and cancellation cause.
///
/// Application code cancels a scope and observes it. Registering a parked
/// waiter, choosing a cause and deciding what a scope absorbs stay internal:
/// each is a bookkeeping invariant of the node that owns the waiter.
struct ScopeAccess {
    static void cancel(CancelScope& scope, CancelCause cause) noexcept { scope.cancel(cause); }

    static void add_waiter(CancelScope& scope, WaitNode& node) noexcept { scope.add_waiter(node); }

    static void remove_waiter(CancelScope& scope, WaitNode& node) noexcept { scope.remove_waiter(node); }

    [[nodiscard]] static bool absorbs(const CancelScope& scope, const Cancelled& error) noexcept {
        return scope.absorbs(error);
    }

    /// Timer action for the timeout and deadline combinators.
    static void timeout_action(void* context) noexcept;
};

/// Resumes a parked frame through the loop's ready queue. A frame with no loop
/// was never started, so there is nothing to wake.
inline void wake(PromiseBase& frame) noexcept {
    if (frame.loop != nullptr) {
        LoopAccess::schedule(*frame.loop, frame);
    }
}

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_ACCESS_H