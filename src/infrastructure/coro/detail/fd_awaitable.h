// Coroutine runtime — the poll() awaitable behind wait_readable / wait_writable.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H

#include <stdexcept>

#include <coroutine>

#include <poll.h>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/detail/wait_node.h"
#include "infrastructure/coro/loop.h"

namespace coro::detail {

/// Awaits one direction of readiness on a file descriptor.
///
/// Ownership: borrows `fd`; the caller keeps the descriptor open until the await
/// completes. The registration is released before the awaiting frame is resumed,
/// so a resume can never race the next poll() round.
/// Cancellation: a checkpoint — a cancelled scope resumes the frame early with
/// `coro::Cancelled` and leaves the descriptor untouched.
/// Failure: throws `Cancelled` on cancellation; a missing loop or invalid
/// descriptor throws `std::logic_error`.
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

    constexpr bool await_ready() const noexcept { return false; }

    /// Registers the fd for `events` and parks. Allocates (fd table growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        if (loop_ == nullptr || fd_ < 0) {
            throw std::logic_error("fd wait requires a running loop and an open descriptor");
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        frame_ = &promise;
        node_.waiter = &promise;
        node_.owner = this;
        node_.on_cancel = &FdAwaitable::on_cancel;
        token_ = LoopAccess::add_fd(*loop_, fd_, events_, &FdAwaitable::on_ready, this);
        armed_ = true;
        if (scope_ != nullptr) {
            ScopeAccess::add_waiter(*scope_, node_);
        }
        return true;
    }

    /// Returns nothing once the direction is ready, `coro::Cancelled` when
    /// the scope cancelled the wait. Cancellation throws `Cancelled`; the fd is untouched.
    void await_resume() {
        if (node_.linked && node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*node_.scope, node_);
        }
        if (armed_) {
            LoopAccess::remove_fd(*loop_, token_);
            armed_ = false;
        }
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
    }

private:
    FdAwaitable(int fd, short events) noexcept : fd_(fd), events_(events) {}

    /// Loop dispatch: unregister first (so the next poll() round cannot fire this
    /// registration twice), then wake the frame through the ready queue.
    static void on_ready(void* context, short /*revents*/) noexcept {
        auto* self = static_cast<FdAwaitable*>(context);
        if (self->armed_) {
            LoopAccess::remove_fd(*self->loop_, self->token_);
            self->armed_ = false;
        }
        if (self->node_.linked && self->node_.scope != nullptr) {
            ScopeAccess::remove_waiter(*self->node_.scope, self->node_);
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
            LoopAccess::remove_fd(*self->loop_, self->token_);
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
    bool armed_ = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H