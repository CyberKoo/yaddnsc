//
// Coroutine runtime — cancel scopes.
//
// Cancellation is a property of a scope, not of a coroutine. A scope is a node
// in a tree: every running frame has an innermost scope, combinators
// (with_timeout / with_deadline / with_cancel_scope / with_nursery) create child
// scopes, and an await registers a waiter on the innermost scope before
// parking. Cancelling a scope sets its flag and wakes every waiter in its
// (unshielded) subtree; nothing uses file descriptors, pipes or a second
// cancellation domain.
//
// The public surface is the four operations a caller performs on a scope it
// was handed — observe, cancel, and observe cancellation again — plus the
// constructor the runtime uses to build the tree. Subscription membership, cause
// selection and absorption are private and reached through detail::ScopeAccess,
// because each is an invariant of the node that owns the subscription.
//

#ifndef YADDNSC_CORO_CANCEL_SCOPE_H
#define YADDNSC_CORO_CANCEL_SCOPE_H

#include "coro/detail/intrusive_list.hpp"

namespace coro {
class Cancelled;

namespace detail {
class CancelSubscription;
enum class CancelCause;
struct ScopeAccess;
}  // namespace detail

/// A node in the cancel-scope tree.
///
/// Lifetime: a scope must outlive every frame that runs inside it, because
/// frames subscribe their wait operations here. Both the combinators and the
/// group create their scope as a local of the combinator's own frame and join
/// their body before returning, which is what makes that guarantee structural.
/// Thread safety: loop thread only; a scope and its subtree are never touched
/// from another thread.
class CancelScope {
public:
    /// `parent` may be null for the root scope; `shielded` marks a scope that
    /// ignores ancestor cancellation (see non_cancellable). Registration is
    /// allocation-free; the parent must outlive this scope.
    explicit CancelScope(CancelScope* parent = nullptr, bool shielded = false) noexcept;

    /// Detaches from the parent. Precondition: no subscription or child scope
    /// remains (a scope is destroyed only after its subtree has finished).
    /// Violations terminate in production, with assertions for Debug diagnosis.
    ~CancelScope() noexcept;

    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

    /// True when this scope or any unshielded ancestor is cancelled.
    [[nodiscard]] bool cancelled() const noexcept;

    /// True when this scope's own deadline initiated cancellation.
    /// A later deadline does not replace an earlier manual cancellation.
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }

    /// Cancel this scope and wake every waiter in its unshielded subtree.
    ///
    /// Idempotent. Waiters are only enqueued, never resumed inline. Never
    /// throws: it sets flags, unlinks nodes and calls no-throw cancellation
    /// hooks. Traversal is iterative and allocation-free regardless of depth.
    /// Hooks (including worker stop callbacks) may run synchronously; they
    /// must not block, destroy scopes or re-enter the runtime.
    void cancel() noexcept;

    /// Throw `Cancelled` when this scope or an unshielded ancestor is
    /// cancelled, and return normally otherwise.
    ///
    /// This is how a caller turns an observed cancellation into control flow:
    /// the plugin gateway checks `cancelled()` and then this to propagate a
    /// host-originated abort as coroutine cancellation. Cancellation is
    /// sticky, so every later checkpoint observes it again.
    void throw_if_cancelled() const;

private:
    friend struct detail::ScopeAccess;

    /// Runtime-only: absorb only our cancellation, never an active ancestor's.
    [[nodiscard]] bool absorbs(const Cancelled& error) const noexcept;

    /// Runtime-only cause selection; callers request cancellation with cancel().
    void cancel(detail::CancelCause cause) noexcept;

    /// Runtime-only: link a subscription. The subscriber keeps the subscription
    /// alive and disarms it; a scope only ever holds borrowed links.
    void add_subscriber(detail::CancelSubscription& subscription) noexcept;

    [[nodiscard]] const CancelScope* cancellation_origin() const noexcept;

    void wake_subtree() noexcept;
    void wake_subscriptions() noexcept;

    // Borrowed intrusive tree links; scopes remain owned by their frames.
    // Appending children preserves registration order during cancellation.
    CancelScope* parent_ = nullptr;
    CancelScope* children_head_ = nullptr;
    CancelScope* children_tail_ = nullptr;
    // Neighbours may be const observer scopes; membership is bookkeeping.
    mutable CancelScope* next_sibling_ = nullptr;
    mutable CancelScope* prev_sibling_ = nullptr;
    // Cancellation subscriptions, owned by their subscribers. Popped one at a
    // time during cancellation: a callback may legally disarm others, so no
    // cursor may survive across it.
    detail::IntrusiveList<detail::CancelSubscription> subscriptions_;
    bool cancelled_ = false;
    bool timed_out_ = false;
    bool shielded_ = false;
};

}  // namespace coro

#endif  // YADDNSC_CORO_CANCEL_SCOPE_H