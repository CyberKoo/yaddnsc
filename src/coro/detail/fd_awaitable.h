// Coroutine runtime — the poll() awaitable behind wait_readable / wait_writable.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H

#include <cassert>
#include <stdexcept>

#include <coroutine>

#include <poll.h>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/wait_node.h"
#include "coro/loop.h"

namespace coro::detail {

/// Awaits one direction of readiness on a file descriptor.
///
/// Precondition: awaited inside coro::run (the frame must run on a loop).
/// Ownership: borrows `fd`; the caller keeps the descriptor open until the await
/// completes. The registration is released before the awaiting frame is resumed,
/// so a resume can never race the next poll() round.
/// Cancellation: a checkpoint — a cancelled scope resumes the frame early with
/// `coro::Cancelled` and leaves the descriptor untouched.
/// Failure: throws `Cancelled` on cancellation; an invalid descriptor throws
/// `std::invalid_argument`.
/// Thread safety: loop thread only.
class FdAwaiter {
public:
    FdAwaiter(const FdAwaiter&) = delete;
    FdAwaiter& operator=(const FdAwaiter&) = delete;
    FdAwaiter(FdAwaiter&&) = delete;
    FdAwaiter& operator=(FdAwaiter&&) = delete;
    // Started frames drain before destruction; readiness/cancellation removes
    // registration before resume. Destroying a parked frame is not supported.
    ~FdAwaiter() = default;

    constexpr bool await_ready() const noexcept { return false; }

    /// Registers the fd for `events` and parks. Allocates (fd table growth), so
    /// it may throw before the frame is parked.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        loop_ = promise.loop;
        scope_ = promise.scope;
        assert(loop_ != nullptr && "wait_readable()/wait_writable() must be awaited inside coro::run");
        if (fd_ < 0) {
            throw std::invalid_argument("fd wait requires an open descriptor");
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        frame_ = &promise;
        node_.waiter = &promise;
        node_.owner = this;
        node_.on_cancel = &FdAwaiter::on_cancel;
        token_ = LoopAccess::add_fd(*loop_, fd_, events_, &FdAwaiter::on_ready, this);
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
        if ((revents_ & POLLNVAL) != 0) {
            throw std::invalid_argument("fd wait requires an open descriptor");
        }
    }

public:
    FdAwaiter(int fd, short events) noexcept : fd_(fd), events_(events) {}

private:
    /// Loop dispatch: unregister first (so the next poll() round cannot fire this
    /// registration twice), then wake the frame through the ready queue.
    static void on_ready(void* context, short revents) noexcept {
        auto* self = static_cast<FdAwaiter*>(context);
        self->revents_ = revents;
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
        auto* self = static_cast<FdAwaiter*>(node.owner);
        if (self->armed_) {
            LoopAccess::remove_fd(*self->loop_, self->token_);
            self->armed_ = false;
        }
    }

    int fd_ = -1;
    short events_ = 0;
    short revents_ = 0;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    PromiseBase* frame_ = nullptr;
    FdToken token_ = 0;
    WaitNode node_{};
    bool armed_ = false;
};

/// Reusable readiness operation; borrows the fd until each await finishes.
class FdAwaitable {
public:
    static FdAwaitable readable(int fd) noexcept { return FdAwaitable{fd, POLLIN}; }

    static FdAwaitable writable(int fd) noexcept { return FdAwaitable{fd, POLLOUT}; }

    [[nodiscard]] FdAwaiter operator co_await() const noexcept { return FdAwaiter{fd_, events_}; }

private:
    FdAwaitable(int fd, short events) noexcept : fd_(fd), events_(events) {}

    int fd_;
    short events_;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FD_AWAITABLE_H
