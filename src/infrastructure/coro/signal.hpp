//
// Coroutine runtime — signals as coroutine events.
//
// on_signal parks the awaiting frame; the process-wide sigaction handler only
// latches a pending bit and writes one byte to the loop's self-pipe, and the
// loop turns that into an ordinary coroutine resumption. There is no signal
// thread and no second notification domain. Users loop for repeated handling.
//
// Process-wide state: the handler cannot capture, so it reaches its loop
// through two file-scope atomics declared in loop.cpp. See the justification
// there.
//
// The wait is cancellable (it registers on the awaiting scope), so a scope
// timeout can end a pending signal wait; that is why the result carries the
// cancellation as a value rather than being Task<void>.
//

#ifndef YADDNSC_CORO_SIGNAL_HPP
#define YADDNSC_CORO_SIGNAL_HPP

#include <system_error>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/task.hpp"

namespace coro {

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

    bool await_ready() const noexcept { return false; }

    /// Installs the handler if needed and parks. Allocates (loop's waiter
    /// list), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr) {
            cancelled_ = true;
            return false;
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            cancelled_ = true;
            return false;
        }
        node_.waiter = &promise;
        node_.cancelled_flag = &cancelled_;
        node_.owner = this;
        node_.on_cancel = &SignalAwaitable::on_cancel;
        if (scope_ != nullptr) {
            scope_->add_waiter(node_);
        }
        loop_->arm_signal(sig_, node_, &delivered_);
        return true;
    }

    /// Returns nothing once the signal arrives, `operation_canceled` when the
    /// scope cancelled the wait. Never throws.
    [[nodiscard]] std::expected<void, std::errc> await_resume() noexcept {
        if (node_.linked && node_.scope != nullptr) {
            node_.scope->remove_waiter(node_);
        }
        if (loop_ != nullptr) {
            loop_->disarm_signal(sig_, node_);
        }
        if (cancelled_ || !delivered_) {
            return std::unexpected(std::errc::operation_canceled);
        }
        return {};
    }

private:
    /// Cancellation hook: stop listening for the signal. Never throws.
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<SignalAwaitable*>(node.owner);
        if (self->loop_ != nullptr) {
            self->loop_->disarm_signal(self->sig_, self->node_);
        }
    }

    int sig_ = 0;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    WaitNode node_{};
    bool cancelled_ = false;
    bool delivered_ = false;
};

/// Complete once the process receives `sig`; cancelled if the scope is.
///
/// The signal handler is process-wide and installed for the lifetime of the
/// loop, so a caller must not also rely on the default disposition of `sig`.
/// Failure: allocation may throw.
[[nodiscard]] inline Task<std::expected<void, std::errc>> on_signal(int sig) {
    co_return co_await SignalAwaitable{sig};
}

}  // namespace coro

#endif  // YADDNSC_CORO_SIGNAL_HPP
