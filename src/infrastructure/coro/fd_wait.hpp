//
// Coroutine runtime — file-descriptor readiness waits.
//
// The runtime's fd table is callback-based (Loop::add_fd); this is the awaitable
// that turns one registration into a checkpoint: park the awaiting frame, resume
// it through the ready queue when poll() reports the requested direction, and
// surface a scope cancellation as unexpected(operation_canceled).
//
// This is the smallest increment the transport layer needs from the runtime: no
// timeout parameter (a deadline is the caller's cancel scope), no reactor object
// (`Loop` comes from the ambient context), and no second notification path (the
// registration is dropped before the frame is resumed).
//

#ifndef YADDNSC_CORO_FD_WAIT_HPP
#define YADDNSC_CORO_FD_WAIT_HPP

#include <system_error>

#include <coroutine>

#include <expected>
#include <poll.h>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro {

/// Awaits one direction of readiness on a file descriptor.
///
/// Ownership: borrows `fd`; the caller keeps the descriptor open until the await
/// completes. The registration is released before the awaiting frame is resumed,
/// so a resume can never race the next poll() round.
/// Cancellation: a checkpoint — a cancelled scope resumes the frame early with
/// `operation_canceled` and leaves the descriptor untouched.
/// Failure: `operation_canceled` on cancellation, and also when the primitive is
/// awaited outside a loop or with an invalid descriptor.
/// Thread safety: loop thread only.
class FdAwaitable {
public:
    /// Wait for POLLIN: readable, or EOF/error reported as readable.
    static FdAwaitable readable(int fd) noexcept { return FdAwaitable{fd, POLLIN}; }

    /// Wait for POLLOUT: writable (also how a non-blocking connect completes).
    static FdAwaitable writable(int fd) noexcept { return FdAwaitable{fd, POLLOUT}; }

    FdAwaitable(const FdAwaitable&) = delete;
    FdAwaitable& operator=(const FdAwaitable&) = delete;
    FdAwaitable(FdAwaitable&&) = delete;
    FdAwaitable& operator=(FdAwaitable&&) = delete;
    ~FdAwaitable() = default;

    bool await_ready() const noexcept { return false; }

    /// Registers the fd for `events` and parks. Allocates (fd table growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr || fd_ < 0) {
            cancelled_ = true;  // awaited outside coro::run, or on a closed fd
            return false;
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            cancelled_ = true;
            return false;
        }
        frame_ = &promise;
        node_.waiter = &promise;
        node_.cancelled_flag = &cancelled_;
        node_.owner = this;
        node_.on_cancel = &FdAwaitable::on_cancel;
        token_ = loop_->add_fd(fd_, events_, &FdAwaitable::on_ready, this);
        armed_ = true;
        if (scope_ != nullptr) {
            scope_->add_waiter(node_);
        }
        return true;
    }

    /// Returns nothing once the direction is ready, `operation_canceled` when
    /// the scope cancelled the wait. Never throws, never touches the fd.
    [[nodiscard]] std::expected<void, std::errc> await_resume() noexcept {
        if (node_.linked && node_.scope != nullptr) {
            node_.scope->remove_waiter(node_);
        }
        if (armed_) {
            loop_->remove_fd(token_);
            armed_ = false;
        }
        if (cancelled_) {
            return std::unexpected(std::errc::operation_canceled);
        }
        return {};
    }

private:
    FdAwaitable(int fd, short events) noexcept : fd_(fd), events_(events) {}

    /// Loop dispatch: unregister first (so the next poll() round cannot fire this
    /// registration twice), then wake the frame through the ready queue.
    static void on_ready(void* context, short /*revents*/) noexcept {
        auto* self = static_cast<FdAwaitable*>(context);
        if (self->armed_) {
            self->loop_->remove_fd(self->token_);
            self->armed_ = false;
        }
        if (self->node_.linked && self->node_.scope != nullptr) {
            self->node_.scope->remove_waiter(self->node_);
        }
        if (self->frame_ != nullptr) {
            wake(*self->frame_);
        }
    }

    /// Cancellation hook: drop the registration so poll() stops reporting it.
    /// Never throws; runs on the loop thread.
    static void on_cancel(WaitNode& node) noexcept {
        auto* self = static_cast<FdAwaitable*>(node.owner);
        if (self->armed_) {
            self->loop_->remove_fd(self->token_);
            self->armed_ = false;
        }
    }

    int fd_ = -1;
    short events_ = 0;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    PromiseBase* frame_ = nullptr;
    FdToken token_ = 0;
    WaitNode node_{};
    bool cancelled_ = false;
    bool armed_ = false;
};

/// Suspend until `fd` is readable (or reports EOF/error as readable).
[[nodiscard]] inline FdAwaitable wait_readable(int fd) noexcept {
    return FdAwaitable::readable(fd);
}

/// Suspend until `fd` is writable.
[[nodiscard]] inline FdAwaitable wait_writable(int fd) noexcept {
    return FdAwaitable::writable(fd);
}

}  // namespace coro

#endif  // YADDNSC_CORO_FD_WAIT_HPP
