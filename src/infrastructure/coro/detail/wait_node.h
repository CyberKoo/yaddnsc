// Coroutine runtime — internal cancellation waiter node.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_NODE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_NODE_H

#include "infrastructure/coro/fwd.h"

namespace coro::detail {

struct PromiseBase;

/// Why a scope was cancelled. Only TIMEOUT is observable through the scope.
enum class CancelCause { REQUESTED, TIMEOUT };

/// One parked coroutine registered on a cancel scope.
///
/// The awaiting frame owns this node and keeps it alive until it is resumed or
/// removed. Thread safety: loop thread only.
struct WaitNode {
    WaitNode* next = nullptr;
    WaitNode* prev = nullptr;
    /// Awaiting frame; it must outlive this registration.
    PromiseBase* waiter = nullptr;
    /// Scope that registered the node; retained until the waiter resumes.
    CancelScope* scope = nullptr;
    /// Set when cancellation wins the race with the wait.
    bool* cancelled_flag = nullptr;
    /// Awaiter owning this node; used by the cleanup hook.
    void* owner = nullptr;
    /// Domain-specific cleanup; runs from cancel() on the loop thread and must not throw.
    void (*on_cancel)(WaitNode&) noexcept = nullptr;
    bool linked = false;
    /// Prevents a second schedule when cancellation races with normal completion.
    bool scheduled = false;
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_WAIT_NODE_H
