// Coroutine runtime — the awaitable behind on_signal.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SIGNAL_AWAITABLE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SIGNAL_AWAITABLE_H

#include <stdexcept>

#include <coroutine>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/wait_node.h"
#include "coro/loop.h"

namespace coro::detail {

/// Awaits one delivery of a signal.
///
/// Ownership: registers a wait node on the awaiting scope and an entry in the
/// loop's per-signal list; both are removed on resume, and the frame must stay
/// alive while parked.
/// Cancellation: a checkpoint. The handler is installed on first use and the
/// previous disposition is restored when the loop dies.
/// Thread safety: await_suspend/await_resume run on the loop thread; the
/// handler runs on whatever thread took the signal and only touches atomics.
class SignalAwaitable {
public:
    explicit SignalAwaitable(int sig) noexcept : sig_(sig) {}

    SignalAwaitable(const SignalAwaitable&) = delete;
    SignalAwaitable& operator=(const SignalAwaitable&) = delete;

    constexpr bool await_ready() const noexcept { return false; }

    /// Installs the handler if needed and parks. Allocates (loop's waiter
    /// list), so it may throw. Invalid/uncatchable signals throw
    /// std::invalid_argument; handler installation failure throws std::system_error.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr) {
            throw std::logic_error("signal wait requires a running loop");
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        node_.waiter = &promise;
        node_.owner = this;
        node_.on_cancel = &SignalAwaitable::on_cancel;
        LoopAccess::arm_signal(*loop_, sig_, node_);
        if (scope_ != nullptr) {
            ScopeAccess::add_waiter(*scope_, node_);
        }
        return true;
    }

    /// Returns nothing once the signal arrives, `coro::Cancelled` when the
    /// scope cancelled the wait. Cancellation throws `Cancelled`.
    void await_resume() {
        if (node_.linked && node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*node_.scope, node_);
        }
        if (loop_ != nullptr) {
            LoopAccess::disarm_signal(*loop_, sig_, node_);
        }
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
    }

private:
    /// Cancellation hook: stop listening for the signal. Never throws.
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<SignalAwaitable*>(node.owner);
        if (self->loop_ != nullptr) {
            LoopAccess::disarm_signal(*self->loop_, self->sig_, self->node_);
        }
    }

    int sig_ = 0;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    WaitNode node_{};
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_SIGNAL_AWAITABLE_H