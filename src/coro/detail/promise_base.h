// Coroutine runtime — shared promise-to-execution binding.
#ifndef YADDNSC_CORO_DETAIL_PROMISE_BASE_H
#define YADDNSC_CORO_DETAIL_PROMISE_BASE_H

#include <exception>

#include <coroutine>

#include "coro/cancelled.h"
#include "coro/fwd.h"

namespace coro::detail {

class ExecutionRecord;

/// Shared prefix of every runtime coroutine frame.
///
/// Internal to the runtime: it is the frame header the loop and the scopes
/// operate on, not a type callers construct or name. A frame is created
/// suspended; activation binds `exec` before its first resumption. The record
/// owns its frame and execution context; only the loop's retirement transaction
/// may destroy a started frame.
struct PromiseBase {
    /// Record that owns this frame once it is started, bound by the activation
    /// commit; null while the frame is unstarted.
    ExecutionRecord* exec = nullptr;

    /// No-throw boundary: the captured outcome lands in the bound result slot,
    /// classified there. Inherited by every task promise; the compiler's promise
    /// lookup accepts base-class members. A frame can only reach this after an
    /// activation bound `exec`, so the slot always exists.
    ///
    /// Defined in task_promise.hpp, where the record and the result slot are
    /// complete: this header stays the minimal promise/record connection.
    void unhandled_exception() noexcept;
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_PROMISE_BASE_H
