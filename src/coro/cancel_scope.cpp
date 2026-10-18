//
// Coroutine runtime — cancel scope implementation.
//

#include "cancel_scope.h"

#include <cassert>
#include <exception>

#include "coro/cancelled.h"
#include "coro/detail/access.h"
#include "coro/detail/wait_operation.h"

namespace coro {

CancelScope::CancelScope(CancelScope* parent, bool shielded) noexcept : parent_(parent), shielded_(shielded) {
    if (parent_ != nullptr) {
        prev_sibling_ = parent_->children_tail_;
        if (prev_sibling_ != nullptr) {
            prev_sibling_->next_sibling_ = this;
        } else {
            parent_->children_head_ = this;
        }
        parent_->children_tail_ = this;
    }
}

CancelScope::~CancelScope() noexcept {
    // An armed subscription outlives its scope only if its owner died with the
    // wait still parked; the scope would then keep a link into released storage.
    const bool orphaned = !subscriptions_.empty();
    assert(!orphaned && "cancel scope destroyed while a subscription is still armed");
    if (orphaned) {
        std::terminate();
    }
    assert(children_head_ == nullptr && "cancel scope destroyed before its children");
    if (children_head_ != nullptr) {
        std::terminate();  // live nodes would retain dangling tree/subscription links
    }
    if (parent_ != nullptr) {
        if (prev_sibling_ != nullptr) {
            prev_sibling_->next_sibling_ = next_sibling_;
        } else {
            parent_->children_head_ = next_sibling_;
        }
        if (next_sibling_ != nullptr) {
            next_sibling_->prev_sibling_ = prev_sibling_;
        } else {
            parent_->children_tail_ = prev_sibling_;
        }
    }
}

bool CancelScope::cancelled() const noexcept {
    for (const CancelScope* scope = this; scope != nullptr; scope = scope->parent_) {
        if (scope->cancelled_) {
            return true;
        }
        if (scope->shielded_) {
            return false;
        }
    }
    return false;
}

const CancelScope* CancelScope::cancellation_origin() const noexcept {
    const CancelScope* origin = nullptr;
    for (const CancelScope* scope = this; scope != nullptr; scope = scope->parent_) {
        if (scope->cancelled_) {
            origin = scope;
        }
        if (scope->shielded_) {
            break;
        }
    }
    return origin;
}

void CancelScope::throw_if_cancelled() const {
    if (const CancelScope* origin = cancellation_origin()) {
        throw Cancelled{origin};
    }
}

bool CancelScope::absorbs(const Cancelled& error) const noexcept {
    return error.origin_ == this && cancellation_origin() == this;
}

void CancelScope::add_subscriber(detail::CancelSubscription& subscription) noexcept {
    assert(!subscription.armed() && "cancellation subscription already armed");
    // Push front: a callback may walk the remaining chain, and cancellation
    // order across scopes stays registration order within one scope.
    subscriptions_.push_front(subscription.hook_);
}

void detail::SubscriptionAccess::subscribe(CancelScope& scope, detail::CancelSubscription& subscription, void* context,
                                           detail::CancelAction (*on_cancel)(void*) noexcept) {
    // Already cancelled: throw before the subscription exists, so a failed start
    // leaves nothing armed and no event source running.
    scope.throw_if_cancelled();
    subscription.arm(scope, context, on_cancel);
}

void detail::CancelSubscription::arm(CancelScope& scope, void* context,
                                     detail::CancelAction (*on_cancel)(void*) noexcept) noexcept {
    const bool valid = !armed() && on_cancel != nullptr;
    assert(valid && "invalid cancellation subscription arm");
    if (!valid) {
        std::terminate();
    }
    context_ = context;
    on_cancel_ = on_cancel;
    if (scope.cancelled()) {
        // Already cancelled: notify once, inline, and stay unarmed. A pure
        // observer depends on this to fire during construction.
        on_cancel_(context);
        return;
    }
    detail::ScopeAccess::add_subscriber(scope, *this);
}

void CancelScope::cancel(detail::CancelCause cause) noexcept {
    if (cancelled_) {
        return;
    }
    timed_out_ = cause == detail::CancelCause::TIMEOUT;
    cancelled_ = true;
    wake_subtree();
}

void CancelScope::wake_subtree() noexcept {
    CancelScope* scope = this;
    for (;;) {
        scope->wake_subscriptions();
        CancelScope* child = scope->children_head_;
        while (child != nullptr && child->shielded_) {
            child = child->next_sibling_;
        }
        if (child != nullptr) {
            scope = child;
            continue;
        }
        // Walk back up without a recursion stack, skipping shielded siblings
        // and never crossing the initiating scope's subtree boundary.
        while (scope != this) {
            CancelScope* sibling = scope->next_sibling_;
            while (sibling != nullptr && sibling->shielded_) {
                sibling = sibling->next_sibling_;
            }
            if (sibling != nullptr) {
                scope = sibling;
                break;
            }
            scope = scope->parent_;
        }
        if (scope == this) {
            return;
        }
    }
}

void CancelScope::wake_subscriptions() noexcept {
    // Pop → notify → pop. A callback may legally disarm other subscriptions
    // (CancelObserver, an event adapter dropping its own registration), so no
    // cursor may be cached across it: every step re-reads the head of the chain.
    while (detail::CancelSubscription* subscription = subscriptions_.pop_front()) {
        subscription->notify();
    }
}

void detail::ScopeAccess::timeout_action(void* context) noexcept {
    static_cast<CancelScope*>(context)->cancel(detail::CancelCause::TIMEOUT);
}

void CancelScope::cancel() noexcept {
    cancel(detail::CancelCause::REQUESTED);
}

}  // namespace coro
