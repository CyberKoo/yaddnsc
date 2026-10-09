// Coroutine runtime — internal frame header.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FRAME_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FRAME_H

#include <exception>

#include <coroutine>

#include "infrastructure/coro/fwd.h"

namespace coro::detail {

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
    /// Captured defect or cancellation; cancellation is not a child defect.
    std::exception_ptr error{};
    bool cancellation = false;
    /// Intrusive ready-queue membership marker.
    bool in_ready = false;
    /// True only for the task handed to coro::run().
    bool is_root = false;
    /// Set when the context was assigned explicitly and must not be inherited.
    bool context_bound = false;

    /// True when the frame captured a defect.
    [[nodiscard]] bool failed() const noexcept { return static_cast<bool>(error) && !cancellation; }
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_FRAME_H
