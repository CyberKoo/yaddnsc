// Coroutine runtime — awaitables behind current_time(), checkpoint() and
// current_scope(). Each uses the awaiting frame's runtime context.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_AWAITABLES_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_AWAITABLES_H

#include <cassert>

#include <coroutine>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/loop.h"
#include "coro/time.h"

namespace coro::detail {

/// Reads the loop clock without suspending.
///
/// Precondition: awaited inside coro::run (the frame must run on a loop).
class CurrentTime {
public:
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        PromiseBase& promise = handle.promise();
        assert(promise.loop != nullptr && "current_time() must be awaited inside coro::run");
        now_ = promise.loop->now();
        return false;  // never actually suspends
    }

    [[nodiscard]] TimePoint await_resume() const noexcept { return now_; }

private:
    TimePoint now_{};
};

/// Yields to the loop and observes sticky cancellation.
///
/// The frame is re-queued rather than resumed inline, so a checkpoint cannot
/// deepen the stack.
class CheckpointAwaitable {
public:
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& frame = handle.promise();
        assert(frame.loop != nullptr && "checkpoint() must be awaited inside coro::run");
        scope_ = frame.scope;
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        LoopAccess::schedule(*frame.loop, frame);
    }

    void await_resume() const {
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
    }

private:
    CancelScope* scope_ = nullptr;
};

/// Reads the innermost cancel scope of the awaiting frame.
///
/// This is what a caller uses to reach the scope it is already running in; it
/// deliberately yields no loop, no clock and no frame, so the runtime's
/// implicit context stays runtime-internal.
class CurrentScope {
public:
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        scope_ = handle.promise().scope;
        return false;  // never actually suspends
    }

    [[nodiscard]] CancelScope& await_resume() const noexcept {
        assert(scope_ != nullptr && "current_scope() must be awaited inside coro::run");
        return *scope_;
    }

private:
    CancelScope* scope_ = nullptr;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_CONTEXT_AWAITABLES_H