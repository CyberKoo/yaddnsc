// Coroutine runtime — the poll() awaitable behind wait_readable / wait_writable.
#ifndef YADDNSC_CORO_DETAIL_FD_AWAITABLE_H
#define YADDNSC_CORO_DETAIL_FD_AWAITABLE_H

#include <cassert>
#include <coroutine>
#include <stdexcept>
#include <system_error>

#include <poll.h>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/fd_entry.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/wait_operation.h"
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
/// Failure: the wait resolves to the loop's poll errno when the event source
/// itself is impaired; throws `Cancelled` on cancellation; an invalid
/// descriptor is a contract defect and throws `std::invalid_argument`.
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
        assert(promise.exec != nullptr && "fd readiness must be awaited inside coro::run");
        loop_ = promise.exec->loop();
        scope_ = promise.exec->scope();
        assert(loop_ != nullptr && "wait_readable()/wait_writable() must be awaited inside coro::run");
        if (fd_ < 0) {
            throw std::invalid_argument("fd wait requires an open descriptor");
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            return false;
        }
        wait_.begin(*promise.exec);
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &FdAwaiter::on_cancel);
        }
        entry_.fd = fd_;
        entry_.events = events_;
        entry_.fn = &FdAwaiter::on_ready;
        entry_.context = this;
        try {
            LoopAccess::add_fd(*loop_, entry_);
        } catch (...) {
            // The registration was not linked: undo the wait binding only.
            wait_.rollback();
            throw;
        }
        wait_.commit_suspend();
        return true;
    }

    /// Resolution: an empty error_code once the direction is ready, or the
    /// loop's poll errno when the event source itself failed the wait. A
    /// socket-level POLLERR with a healthy loop is left for the next syscall
    /// on the fd to report. Cancellation throws `Cancelled`. A descriptor that
    /// was closed while the wait was parked resolves to
    /// std::errc::bad_file_descriptor, so every failure of this wait arrives
    /// through the same error_code channel.
    [[nodiscard]] std::error_code await_resume() {
        wait_.begin_resume();
        wait_.finish();
        LoopAccess::remove_fd(*loop_, entry_);
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        if ((revents_ & POLLNVAL) != 0) {
            return std::make_error_code(std::errc::bad_file_descriptor);
        }
        if ((revents_ & POLLERR) != 0) {
            const int error = LoopAccess::poll_error(*loop_);
            if (error != 0) {
                return std::error_code(error, std::generic_category());
            }
        }
        return {};
    }

public:
    FdAwaiter(int fd, short events) noexcept : fd_(fd), events_(events) {}

private:
    /// Loop dispatch: unregister first (so the next poll() round cannot fire this
    /// registration twice), then report the completion. The wait operation
    /// arbitrates, so a dispatch that races a cancellation still resumes once.
    static void on_ready(void* context, short revents) noexcept {
        auto* self = static_cast<FdAwaiter*>(context);
        self->revents_ = revents;
        LoopAccess::remove_fd(*self->loop_, self->entry_);
        self->wait_.event_ready();
    }

    /// Cancellation hook: drop the registration so poll() stops reporting it,
    /// and take the resume. Never throws; runs on the loop thread.
    static detail::CancelAction on_cancel(void* context) noexcept {
        auto* self = static_cast<FdAwaiter*>(context);
        LoopAccess::remove_fd(*self->loop_, self->entry_);
        return detail::CancelAction::RESUME;
    }

    int fd_ = -1;
    short events_ = 0;
    short revents_ = 0;
    Loop* loop_ = nullptr;
    CancelScope* scope_ = nullptr;
    FdEntry entry_{};
    CancelSubscription subscription_{};
    detail::WaitOperation wait_{subscription_};
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

#endif  // YADDNSC_CORO_DETAIL_FD_AWAITABLE_H
