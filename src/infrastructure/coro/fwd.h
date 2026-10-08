//
// Coroutine runtime — shared frame prefix and forward declarations.
//
// Every coroutine in the runtime (Task<T> and the internal helper coroutines)
// uses a promise type derived from PromiseBase. The base carries the links the
// loop needs (ready queue, continuation, completion hook) plus the implicit
// context that flows down the task tree: the loop and the innermost cancel
// scope. Nothing else is passed through task signatures.
//

#ifndef YADDNSC_CORO_FWD_H
#define YADDNSC_CORO_FWD_H

#include <exception>

#include <coroutine>

namespace coro {

class CancelScope;
class Clock;
class Loop;
class TaskGroup;

/// Shared prefix of every runtime coroutine frame.
///
/// Internal to the runtime: it is the frame header the loop and the scopes
/// operate on, not a type callers construct or name. A frame is created
/// suspended; its context is filled in either by the awaiting frame (inline
/// `co_await task`) or by the owner that started it (root task, group child),
/// and its lifetime is owned by exactly one `Task<T>`, `TaskAwaiter<T>` or
/// group slot at a time.
struct PromiseBase {
    /// Type-erased handle back to this frame, valid for resume()/destroy().
    std::coroutine_handle<> self{};
    /// Intrusive ready-queue link; a frame is queued at most once.
    PromiseBase* ready_next = nullptr;
    /// Intrusive link for group waiter lists (join / next()).
    PromiseBase* wait_next = nullptr;
    /// Frame to resume when this frame completes (null for the root task and
    /// for group children, which are woken through `completion`).
    PromiseBase* continuation = nullptr;
    /// Completion hook: called with an owner pointer when the frame finishes.
    /// Runs on the loop thread, inside the frame's final suspend, and must not
    /// throw (it cannot allocate).
    void* completion_owner = nullptr;
    void (*completion)(void*, PromiseBase&) noexcept = nullptr;
    /// Implicit context.
    Loop* loop = nullptr;
    CancelScope* scope = nullptr;
    /// Captured exception (defects only; cancellation travels as a value).
    std::exception_ptr error{};
    /// Intrusive ready-queue membership marker.
    bool in_ready = false;
    /// True only for the task handed to coro::run().
    bool is_root = false;
    /// Set when the context was assigned explicitly and must not be inherited.
    bool context_bound = false;

    /// True when the frame captured a defect.
    [[nodiscard]] bool failed() const noexcept { return static_cast<bool>(error); }
};

}  // namespace coro

#endif  // YADDNSC_CORO_FWD_H
