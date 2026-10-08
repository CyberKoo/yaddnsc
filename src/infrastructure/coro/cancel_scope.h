//
// Coroutine runtime — cancel scopes.
//
// Cancellation is a property of a scope, not of a coroutine. A scope is a node
// in a tree: every running frame has an innermost scope, combinators
// (with_timeout / with_cancel_scope / task_group) create child scopes, and an
// await registers a WaitNode on the innermost scope before parking. Cancelling
// a scope sets its flag and wakes every waiter in its (unshielded) subtree;
// nothing uses file descriptors, pipes or a second cancellation domain.
//
// Deliberate conflict with rule 02 ("the per-operation cancellation token must
// be passed per operation, not stored as a member"): this runtime has no
// cancellation tokens by design. Cancellation is scope state reached through
// checkpoints, so there is nothing to pass per operation — see
// .cache/coro_redesign.md §3.3. No token type is referenced anywhere here.
//
// A parked awaiter remains valid until it is resumed, so a WaitNode may hold a
// raw pointer to the awaiting frame plus a flag to set when cancellation wins.
//

#ifndef YADDNSC_CORO_CANCEL_SCOPE_H
#define YADDNSC_CORO_CANCEL_SCOPE_H

#include <cstddef>
#include <vector>

#include "infrastructure/coro/fwd.h"

namespace coro {

/// Why a scope was cancelled. Only `TIMEOUT` is observable through the scope;
/// the distinction exists so `timed_out()` can be answered without translation.
enum class CancelCause { REQUESTED, TIMEOUT, SHUTDOWN };

/// One parked coroutine registered on a cancel scope.
///
/// Ownership: borrows the awaiting frame; the frame owns the node (it lives in
/// the awaiter, i.e. in the frame) and must outlive the registration. `owner`
/// points at the awaiter for domain cleanup; `cancelled_flag` and `waiter` must
/// stay valid until the node is resumed or removed.
/// Thread safety: loop thread only.
struct WaitNode {
    WaitNode* next = nullptr;
    WaitNode* prev = nullptr;
    /// Frame to resume when the wait is satisfied or cancelled.
    PromiseBase* waiter = nullptr;
    /// Scope the node is currently linked into (null when unlinked).
    CancelScope* scope = nullptr;
    /// Set to true when cancellation wins the race with the wait.
    bool* cancelled_flag = nullptr;
    void* owner = nullptr;
    /// Domain-specific cleanup (dequeue a mutex waiter, drop a timer, ...).
    /// Runs on the loop thread from cancel(); must not throw.
    void (*on_cancel)(WaitNode&) = nullptr;
    bool linked = false;
    /// True once some path has already scheduled the waiter (prevents a
    /// double schedule when cancellation races with normal completion).
    bool scheduled = false;
};

/// A node in the cancel-scope tree.
///
/// Lifetime: a scope must outlive every frame that runs inside it, because
/// frames park WaitNodes here. Both the combinators and the group create their
/// scope as a local of the combinator's own frame and join their body before
/// returning, which is what makes that guarantee structural.
/// Thread safety: loop thread only; a scope and its subtree are never touched
/// from another thread.
class CancelScope {
public:
    /// `parent` may be null for the root scope; `shielded` marks a scope that
    /// ignores ancestor cancellation (see non_cancellable).
    explicit CancelScope(CancelScope* parent = nullptr, bool shielded = false);

    /// Detaches from the parent. Precondition: no waiter is still parked here
    /// (a scope is destroyed only after its subtree has finished).
    ~CancelScope() noexcept;

    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

    /// True when this scope or any unshielded ancestor is cancelled.
    [[nodiscard]] bool cancelled() const noexcept;

    /// True when this scope itself was cancelled (used for scope absorption).
    [[nodiscard]] bool cancelled_self() const noexcept { return cancelled_; }

    /// True when this scope's own deadline fired (never set by an outer cancel).
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }

    /// True when this scope ignores ancestor cancellation (non_cancellable).
    [[nodiscard]] bool shielded() const noexcept { return shielded_; }

    /// Parent scope, or null for the root scope. Borrowed.
    [[nodiscard]] CancelScope* parent() const noexcept { return parent_; }

    /// Cancel this scope and wake every waiter in its unshielded subtree.
    ///
    /// Idempotent; `TIMEOUT` additionally latches timed_out(). Waking only
    /// enqueues the waiters on the ready queue, so this never resumes a frame
    /// inline and never runs user code. Never throws: it only sets flags,
    /// unlinks nodes and calls the no-throw on_cancel hooks.
    void cancel(CancelCause cause = CancelCause::REQUESTED) noexcept;

    /// Register a parked awaiter. The caller keeps `node` alive until it is
    /// resumed or removed; a node may be registered on one scope at a time.
    void add_waiter(WaitNode& node) noexcept;
    /// Unlink a waiter. Idempotent, so both the normal-completion and the
    /// cancellation path can call it.
    void remove_waiter(WaitNode& node) noexcept;

    /// TimerNode action used by with_timeout / with_deadline.
    static void timeout_action(void* scope) noexcept;

private:
    void wake_subtree() noexcept;
    void unlink(WaitNode& node) noexcept;

    CancelScope* parent_ = nullptr;
    std::vector<CancelScope*> children_;
    WaitNode* waiters_head_ = nullptr;
    bool cancelled_ = false;
    bool timed_out_ = false;
    bool shielded_ = false;
};

}  // namespace coro

#endif  // YADDNSC_CORO_CANCEL_SCOPE_H
