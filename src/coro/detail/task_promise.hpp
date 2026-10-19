// Coroutine runtime — Task<T> promise, awaiter and ownership access.
//
// These are the runtime's own frame machinery. Nothing here is named by a
// caller except Task::promise_type, which the compiler needs spelled out.
#ifndef YADDNSC_CORO_DETAIL_TASK_PROMISE_HPP
#define YADDNSC_CORO_DETAIL_TASK_PROMISE_HPP

#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/detail/access.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/wait_operation.h"
#include "coro/fwd.h"
#include "coro/loop.h"

namespace coro::detail {

/// The captured outcome goes to the slot the activation bound. A frame reaches
/// this only after its record exists, so the slot is always there, and a
/// cancellation is classified as cancellation rather than as a defect.
inline void PromiseBase::unhandled_exception() noexcept {
    exec->result()->capture_exception(std::current_exception());
}

struct TaskAccess;
template<typename T>
class TaskAwaiter;

/// Shared final_suspend: hand the record to the loop's terminal entry, which
/// marks the execution and queues it for retirement. Nothing else happens here —
/// the value stays in its result slot, the frame stays alive, and no consumer is
/// resumed before the reap transaction published the result.
struct FinalSuspend {
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        PromiseBase& promise = handle.promise();
        LoopAccess::mark_terminal(*promise.exec->loop(), *promise.exec);
    }

    void await_resume() const noexcept {}
};

template<typename T>
struct TaskPromise;

/// Quiet completion of an inline child: the only event allowed to resume a
/// parent parked on `co_await task` (I4). Neither termination nor an ancestor
/// cancellation may wake that parent early.
inline void resume_parent_on_retire(void* context, ExecutionRecord&) noexcept {
    static_cast<WaitCore*>(context)->event_ready();
}

/// The infallible half of every activation, shared by all three entries.
///
/// It binds what can no longer be undone — promise, record, result slot,
/// context, execution tree and registry — so that the caller only has to
/// transfer the frame and queue the first resumption. It never throws, never
/// allocates and never calls user code: an entry that has not reached this call
/// has started nothing and can simply drop the task.
struct Activation {
    /// Bind `record` to its frame's promise and result slot, install the
    /// context and the completion roles, and enter the execution tree and the
    /// loop registry. `parent` is null for a root execution.
    template<auto ON_TERMINAL = nullptr, auto ON_RETIRED = nullptr>
    static void commit(PromiseBase& promise, ExecutionRecord& record, ResultStateBase& result, Loop& loop,
                       CancelScope* scope, ExecutionRecord* parent, void* completion_context) noexcept {
        promise.exec = &record;
        if (record.loop() == nullptr) {
            record.bind_context(loop, scope);
        } else {
            const bool matches = record.loop() == &loop && record.scope() == scope;
            assert(matches && "activation disagrees with prepared context");
            if (!matches) {
                std::terminate();
            }
        }
        record.bind_result(result);
        record.bind_completion<ON_TERMINAL, ON_RETIRED>(completion_context);
        if (parent != nullptr) {
            record.attach_to(*parent);
        }
        LoopAccess::register_execution(loop, record);
    }
};

/// Promise for Task<T>; the result lives in a slot the activation bound before
/// the first resumption, never in the frame.
///
/// Internal: only the compiler and the runtime name it. `initial_suspend`
/// suspends, so calling a task function never runs its body inline.
template<typename T>
struct TaskPromise : PromiseBase {
    /// Pre-bound result slot. Written by return_value() and
    /// unhandled_exception(), published by the loop's reap transaction.
    ResultState<T>* result = nullptr;

    /// Defined in task.hpp, after Task<T> is complete: it constructs one.
    Task<T> get_return_object();

    /// Lazy task: the frame starts suspended, so calling a task function runs
    /// nothing until the result is awaited.
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: hand the record to the loop's terminal entry.
    FinalSuspend final_suspend() noexcept { return {}; }

    /// Constructs the value straight into the bound slot. May throw if T's move
    /// constructor does: the coroutine's own machinery then routes the failure
    /// to unhandled_exception(), so no success is ever published for a body that
    /// did not finish writing one.
    void return_value(T value) {
        assert(result != nullptr && "return_value() before the activation bound the result slot");
        result->write_value(std::move(value));
    }
};

/// Promise for Task<void>: identical, without a value slot.
template<>
struct TaskPromise<void> : PromiseBase {
    /// Pre-bound result slot; carries the success marker only.
    ResultState<void>* result = nullptr;

    /// Defined in task.hpp, after Task<void> is complete: it constructs one.
    Task<void> get_return_object();

    /// Lazy task: the frame starts suspended (see TaskPromise<T>).
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Completion: hand the record to the loop's terminal entry.
    FinalSuspend final_suspend() noexcept { return {}; }

    void return_void() noexcept {
        assert(result != nullptr && "return_void() before the activation bound the result slot");
        result->write_success();
    }
};

/// Runtime-only access to task ownership, context and the activation entries.
///
/// An entry is the only place a started frame is created: it commits the record
/// binding first and releases the task's handle last, so any failure before the
/// commit leaves the task destroyable and no half-started record behind.
struct TaskAccess {
    template<typename T>
    [[nodiscard]] static auto release(Task<T>& task) noexcept {
        return task.release();
    }

    template<typename T>
    static TaskAwaiter<T> await_in_scope(Task<T>& task, Loop& loop, CancelScope& scope) noexcept;

    /// Root activation: the record and result live in the run() session, the
    /// execution tree starts here and nothing wakes it — the session observes
    /// completion through the loop's exit predicate. `task` must be valid; the
    /// session checks that before calling.
    template<typename T>
    static void start_root(Task<T>& task, ExecutionRecord& record, ResultState<T>& result, Loop& loop,
                           CancelScope& scope) noexcept {
        const auto handle = task.release();
        TaskPromise<T>& promise = handle.promise();
        promise.result = &result;
        Activation::commit(promise, record, result, loop, &scope, nullptr, nullptr);
        record.adopt(handle);
        record.mark_active();
        LoopAccess::schedule(loop, record);
    }

    /// Inline activation (co_await task): the child's record and result live in
    /// the awaiter inside the awaiting frame, and its quiet completion resumes
    /// that frame's record.
    template<typename Awaiter>
    static void start_inline(Awaiter& awaiter, PromiseBase& parent) noexcept {
        auto handle = awaiter.handle_;
        typename Awaiter::PromiseType& promise = handle.promise();
        Loop& loop = awaiter.record_.loop() != nullptr ? *awaiter.record_.loop() : *parent.exec->loop();
        CancelScope* scope = awaiter.record_.loop() != nullptr ? awaiter.record_.scope() : parent.exec->scope();
        awaiter.wait_.begin(*parent.exec);
        promise.result = &awaiter.result_;
        Activation::commit<nullptr, &resume_parent_on_retire>(promise, awaiter.record_, awaiter.result_, loop, scope,
                                                              parent.exec, &awaiter.wait_);
        awaiter.handle_ = {};
        awaiter.record_.adopt(handle);
        awaiter.record_.mark_active();
        LoopAccess::schedule(loop, awaiter.record_);
        awaiter.wait_.commit_suspend();
    }
};

/// Awaiter for an inline `co_await task`.
///
/// Precondition: awaited inside coro::run (the awaiting frame must run on a
/// loop); everything the child parks on is inherited from that frame.
/// Ownership: the child record and its result slot are members of this
/// awaiter, so they live in the awaiting frame and cover the child's whole
/// execution and retirement (I2). Once the child is started, its frame belongs
/// to the record and only the loop's reap transaction may destroy it;
/// awaiting a task is not a cancellation checkpoint — the child belongs to the
/// same scope and observes cancellation at its own awaits.
///
/// The awaiter is immovable: the record and its result slot are bound in place
/// inside the awaiting frame.
template<typename T>
class TaskAwaiter {
public:
    using PromiseType = TaskPromise<T>;
    using Handle = std::coroutine_handle<PromiseType>;

    explicit TaskAwaiter(Handle handle) noexcept : handle_(handle) {}

    TaskAwaiter(Handle handle, Loop& loop, CancelScope& scope) noexcept : handle_(handle) {
        record_.bind_context(loop, &scope);
    }

    TaskAwaiter(TaskAwaiter&&) = delete;
    TaskAwaiter& operator=(TaskAwaiter&&) = delete;
    TaskAwaiter(const TaskAwaiter&) = delete;
    TaskAwaiter& operator=(const TaskAwaiter&) = delete;

    /// An unstarted child frame is still ours to destroy. A started one is not:
    /// it belongs to its record, and this awaiter only ever runs again after
    /// that record retired (the parent cannot resume earlier).
    ~TaskAwaiter() noexcept {
        record_.validate_host_lifetime();
        if (handle_) {
            handle_.destroy();
        }
    }

    constexpr bool await_ready() const noexcept { return false; }

    /// Activates the child in this awaiter's storage and parks the parent. The
    /// parent resumes only when the child's reap transaction retires it.
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) {
        if (!handle_) {
            throw std::logic_error("await requires a valid task");
        }
        assert(parent.promise().exec != nullptr && "task must be awaited inside coro::run");
        TaskAccess::start_inline(*this, parent.promise());
    }

    /// Yields the child's value, or rethrows the outcome that ended it. May
    /// throw: the rethrow path, and T's move constructor.
    T await_resume() {
        wait_.begin_resume();
        wait_.finish();
        ResultState<T>& outcome = result_;
        // The inline protocol has exactly one consumer, so the consumption is
        // committed here — before the payload moves, so a throwing move still
        // ends it.
        outcome.commit_consumed();
        if (outcome.content() != ResultContent::VALUE) {
            std::rethrow_exception(outcome.error());
        }
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return outcome.take_value();
        }
    }

private:
    friend struct TaskAccess;

    /// Storage the activation binds. Neither is copyable nor movable, so they
    /// keep their addresses for the child's whole lifetime.
    Handle handle_{};
    ExecutionRecord record_{};
    ResultState<T> result_{};
    WaitCore wait_{};
};

template<typename T>
TaskAwaiter<T> TaskAccess::await_in_scope(Task<T>& task, Loop& loop, CancelScope& scope) noexcept {
    return TaskAwaiter<T>{task.release(), loop, scope};
}

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_TASK_PROMISE_HPP
