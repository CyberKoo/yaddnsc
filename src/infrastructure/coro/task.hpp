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

#ifndef YADDNSC_CORO_TASK_HPP
#define YADDNSC_CORO_TASK_HPP

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro {

/// Implicit context read from the awaiting frame's promise: the loop the task
/// runs on and the innermost scope whose cancellation governs it.
struct Context {
    Loop* loop = nullptr;
    CancelScope* scope = nullptr;
};

/// Reads the awaiting frame's context without suspending.
///
/// Used by the scope combinators and the task group to learn where they run;
/// `co_await GetContext{}` returns the Context and never parks.
class GetContext {
public:
    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        PromiseBase& promise = handle.promise();
        context_ = Context{promise.loop, promise.scope};
        return false;  // never actually suspends
    }

    [[nodiscard]] Context await_resume() const noexcept { return context_; }

private:
    Context context_{};
};

/// Shared final_suspend: run the completion hook, then wake the continuation
/// (through the ready queue) or stop the loop when the root task finishes.
///
/// No-throw by contract: it only touches intrusive links, schedules, and calls
/// an allocation-free completion hook. The frame stays suspended at its final
/// suspend point afterwards; its owner destroys it.
struct FinalSuspend {
    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        PromiseBase& promise = handle.promise();
        if (promise.completion != nullptr) {
            promise.completion(promise.completion_owner, promise);
        }
        if (promise.continuation != nullptr) {
            promise.continuation->loop->schedule(*promise.continuation);
        } else if (promise.is_root && promise.loop != nullptr) {
            promise.loop->request_stop();
        }
    }

    void await_resume() const noexcept {}
};

template<typename T>
struct TaskPromise;
template<typename T>
class Task;

/// Promise for Task<T>; the frame stores the value (or the captured defect).
///
/// Internal: only the compiler and the runtime name it. `initial_suspend`
/// suspends, so calling a task function never runs its body inline.
template<typename T>
struct TaskPromise : PromiseBase {
    std::optional<T> value;

    Task<T> get_return_object();

    /// Lazy task: the frame starts suspended, so calling a task function runs
    /// nothing until the result is awaited.
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: wake the completion hook and then the continuation.
    FinalSuspend final_suspend() noexcept { return {}; }

    /// Allocates (emplace of T) and may throw if T's move constructor throws.
    void return_value(T result) { value.emplace(std::move(result)); }

    /// No-throw boundary: a defect is captured, never propagated out of resume.
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

/// Promise for Task<void>: identical, without a value slot.
template<>
struct TaskPromise<void> : PromiseBase {
    Task<void> get_return_object();

    /// Lazy task: the frame starts suspended (see TaskPromise<T>).
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: wake the completion hook and then the continuation.
    FinalSuspend final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    /// No-throw boundary: a defect is captured, never propagated out of resume.
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

/// Awaiter for an inline `co_await task`.
///
/// Ownership: takes over the child frame and destroys it on resume, so the
/// awaiter (and therefore the awaiting frame) owns the frame across the
/// suspension. Awaiting a task is not a cancellation checkpoint: the child
/// belongs to the same scope and observes cancellation at its own awaits.
template<typename T>
class TaskAwaiter {
public:
    using PromiseType = TaskPromise<T>;
    using Handle = std::coroutine_handle<PromiseType>;

    TaskAwaiter() = default;

    explicit TaskAwaiter(Handle handle) noexcept : handle_(handle) {}

    TaskAwaiter(TaskAwaiter&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    TaskAwaiter& operator=(TaskAwaiter&&) = delete;
    TaskAwaiter(const TaskAwaiter&) = delete;
    TaskAwaiter& operator=(const TaskAwaiter&) = delete;

    /// Destroys an un-resumed child frame (never leaks the frame).
    ~TaskAwaiter() noexcept {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool await_ready() const noexcept { return false; }

    /// Hands the child to the ready queue; the parent parks and keeps running
    /// nothing until the loop resumes it.
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) noexcept {
        PromiseBase& parent_promise = parent.promise();
        PromiseBase& child = handle_.promise();
        if (!child.context_bound) {
            child.loop = parent_promise.loop;
            child.scope = parent_promise.scope;
        }
        child.continuation = &parent_promise;
        child.loop->schedule(child);
    }

    /// Yields the child's value, or rethrows the defect that aborted it.
    /// May throw: the rethrow path, and T's move constructor when T is moved
    /// out of the frame.
    T await_resume() {
        Handle handle = std::exchange(handle_, {});
        if (handle.promise().failed()) {
            const std::exception_ptr error = handle.promise().error;
            handle.destroy();
            std::rethrow_exception(error);
        }
        if constexpr (std::is_void_v<T>) {
            handle.destroy();
            return;
        } else {
            T result = std::move(*handle.promise().value);
            handle.destroy();
            return result;
        }
    }

private:
    Handle handle_{};
};

/// A lazy coroutine handle that owns its frame.
///
/// Ownership: exactly one Task owns the frame. Moving transfers it; awaiting
/// transfers it to the awaiter; release() hands it to a group or to coro::run.
/// The destructor destroys an un-started or completed frame, so a dropped Task
/// runs nothing and leaks nothing.
/// Failure: a task never throws out of its own body — a defect is captured and
/// rethrown at the co_await point (or read by the group).
/// Thread safety: a Task is not thread-safe; it is created, awaited and
/// destroyed on the loop thread.
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;
    using ValueType = T;

    Task() = default;

    explicit Task(Handle handle) noexcept : handle_(handle) {}

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

    /// Release the frame (ownership transfer to a group or to coro::run).
    [[nodiscard]] Handle release() noexcept { return std::exchange(handle_, {}); }

    /// Pin the implicit context of a not-yet-started task (group bodies).
    void bind_context(Loop& loop, CancelScope& scope) noexcept {
        if (!handle_) {
            return;
        }
        PromiseBase& promise = handle_.promise();
        promise.loop = &loop;
        promise.scope = &scope;
        promise.context_bound = true;
    }

    /// Single-shot: awaiting a task transfers ownership of its frame, so an
    /// lvalue task becomes invalid after the co_await.
    TaskAwaiter<T> operator co_await() noexcept { return TaskAwaiter<T>{release()}; }

private:
    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    Handle handle_{};
};

template<typename T>
Task<T> TaskPromise<T>::get_return_object() {
    const auto handle = std::coroutine_handle<TaskPromise>::from_promise(*this);
    this->self = handle;
    return Task<T>{handle};
}

inline Task<void> TaskPromise<void>::get_return_object() {
    const auto handle = std::coroutine_handle<TaskPromise>::from_promise(*this);
    this->self = handle;
    return Task<void>{handle};
}

}  // namespace coro

#endif  // YADDNSC_CORO_TASK_HPP
