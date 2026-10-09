// Coroutine runtime — Task<T> promise, awaiter and ownership access.
//
// These are the runtime's own frame machinery. Nothing here is named by a
// caller except Task::promise_type, which the compiler needs spelled out.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TASK_PROMISE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TASK_PROMISE_H

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro::detail {

/// Shared final_suspend: run the completion hook, then wake the continuation
/// (through the ready queue) or stop the loop when the root task finishes.
///
/// No-throw by contract: it only touches intrusive links, schedules, and calls
/// an allocation-free completion hook. The frame stays suspended at its final
/// suspend point afterwards; its owner destroys it.
struct FinalSuspend {
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        PromiseBase& promise = handle.promise();
        if (promise.completion != nullptr) {
            promise.completion(promise.completion_owner, promise);
        }
        if (promise.continuation != nullptr) {
            LoopAccess::schedule(*promise.continuation->loop, *promise.continuation);
        } else if (promise.is_root && promise.loop != nullptr) {
            promise.loop->request_stop();
        }
    }

    void await_resume() const noexcept {}
};

template<typename T>
struct TaskPromise;

/// Promise for Task<T>; the frame stores the value (or the captured defect).
///
/// Internal: only the compiler and the runtime name it. `initial_suspend`
/// suspends, so calling a task function never runs its body inline.
template<typename T>
struct TaskPromise : PromiseBase {
    std::optional<T> value;

    /// Defined in task.hpp, after Task<T> is complete: it constructs one.
    Task<T> get_return_object();

    /// Lazy task: the frame starts suspended, so calling a task function runs
    /// nothing until the result is awaited.
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: wake the completion hook and then the continuation.
    FinalSuspend final_suspend() noexcept { return {}; }

    /// Allocates (emplace of T) and may throw if T's move constructor throws.
    void return_value(T result) { value.emplace(std::move(result)); }

    /// No-throw boundary: a defect is captured, never propagated out of resume.
    void unhandled_exception() noexcept {
        error = std::current_exception();
        try {
            std::rethrow_exception(error);
        } catch (const Cancelled&) {
            cancellation = true;
        } catch (...) {
            // All other exceptions retain their defect classification.
        }
    }
};

/// Promise for Task<void>: identical, without a value slot.
template<>
struct TaskPromise<void> : PromiseBase {
    /// Defined in task.hpp, after Task<void> is complete: it constructs one.
    Task<void> get_return_object();

    /// Lazy task: the frame starts suspended (see TaskPromise<T>).
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: wake the completion hook and then the continuation.
    FinalSuspend final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    /// No-throw boundary: a defect is captured, never propagated out of resume.
    void unhandled_exception() noexcept {
        error = std::current_exception();
        try {
            std::rethrow_exception(error);
        } catch (const Cancelled&) {
            cancellation = true;
        } catch (...) {
            // All other exceptions retain their defect classification.
        }
    }
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

    constexpr bool await_ready() const noexcept { return false; }

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
        LoopAccess::schedule(*child.loop, child);
    }

    /// Yields the child's value, or rethrows the defect that aborted it.
    /// May throw: the rethrow path, and T's move constructor when T is moved
    /// out of the frame.
    T await_resume() {
        Handle handle = std::exchange(handle_, {});
        if (handle.promise().error) {
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

/// Runtime-only access to task ownership and context; no application API.
struct TaskAccess {
    template<typename T>
    [[nodiscard]] static auto release(Task<T>& task) noexcept {
        return task.release();
    }

    template<typename T>
    static void bind_context(Task<T>& task, Loop& loop, CancelScope& scope) noexcept {
        task.bind_context(loop, scope);
    }
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_TASK_PROMISE_H