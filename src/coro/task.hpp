//
// Coroutine runtime — Task<T>.
//
// Tasks are lazy: creating one does not run it. `co_await task` runs it inline —
// the parent parks and the child is resumed through the ready queue, so the
// result (or the defect that aborted it) surfaces at the co_await point while
// stack depth stays bounded.
//
// The implicit context (loop + innermost cancel scope) is inherited by every
// awaited task; nothing is threaded through call signatures.
//
// Awaiting outside coro::run is a programming defect: assertion-diagnosed in
// Debug builds, undefined behavior in Release.
//
// The promise, the awaiter and the ownership backdoor live in
// detail/task_promise.hpp. What stays here is the task itself plus the two
// get_return_object() definitions, which have to follow Task<T> because they
// construct one.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_TASK_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_TASK_HPP

#include <coroutine>
#include <utility>

#include "coro/detail/promise_base.h"
#include "coro/detail/task_promise.hpp"  // IWYU pragma: export
#include "coro/fwd.h"
#include "coro/loop.h"

namespace coro {

/// A lazy coroutine handle that owns its frame.
///
/// Ownership: exactly one Task owns the frame. Moving transfers it; awaiting
/// transfers it to an awaiter; activation transfers it to its execution record.
/// The destructor destroys only an unstarted frame, so a dropped Task
/// runs nothing and leaks nothing.
/// Failure: a task never throws out of its own body — a defect is captured and
/// rethrown at the co_await point (or read by the group).
/// Thread safety: a Task is not thread-safe; it is created, awaited and
/// destroyed on the loop thread.
template<typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using ValueType = T;

    Task() = default;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() noexcept { reset(); }

    /// True when this frame owns a coroutine frame.
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    /// Single-shot: awaiting transfers the frame to an internal awaiter.
    /// An awaited lvalue task becomes invalid.
    /// Awaiting an empty or already awaited task throws std::logic_error.
    /// Precondition: awaited inside coro::run (see the header note).
    detail::TaskAwaiter<T> operator co_await() noexcept { return detail::TaskAwaiter<T>{release()}; }

private:
    friend struct detail::TaskPromise<T>;
    friend struct detail::TaskAccess;
    using FrameHandle = std::coroutine_handle<promise_type>;

    explicit Task(FrameHandle handle) noexcept : handle_(handle) {}

    [[nodiscard]] FrameHandle release() noexcept { return std::exchange(handle_, {}); }

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    FrameHandle handle_{};
};

namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() {
    const auto handle = std::coroutine_handle<TaskPromise>::from_promise(*this);
    return Task<T>{handle};
}

inline Task<void> TaskPromise<void>::get_return_object() {
    const auto handle = std::coroutine_handle<TaskPromise>::from_promise(*this);
    return Task<void>{handle};
}

}  // namespace detail
}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_TASK_HPP
