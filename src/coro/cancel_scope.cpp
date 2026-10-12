//
// Coroutine runtime — cancel scope implementation.
//

#include "cancel_scope.h"

#include <cassert>
#include <exception>

#include "coro/cancelled.h"
#include "coro/detail/access.h"
#include "coro/detail/wait_node.h"

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
    assert(waiters_head_ == nullptr && "cancel scope destroyed while a waiter is still parked");
    assert(children_head_ == nullptr && "cancel scope destroyed before its children");
    if (waiters_head_ != nullptr || children_head_ != nullptr) {
        std::terminate();  // live nodes would retain dangling tree/waiter links
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

void CancelScope::unlink(detail::WaitNode& node) noexcept {
    if (!node.linked) {
        return;
    }
    if (node.prev != nullptr) {
        node.prev->next = node.next;
    } else {
        waiters_head_ = node.next;
    }
    if (node.next != nullptr) {
        node.next->prev = node.prev;
    }
    node.next = nullptr;
    node.prev = nullptr;
    node.linked = false;
}

void CancelScope::add_waiter(detail::WaitNode& node) noexcept {
    assert(!node.linked && "wait node already registered");
    node.scope = this;
    node.prev = nullptr;
    node.next = waiters_head_;
    if (waiters_head_ != nullptr) {
        waiters_head_->prev = &node;
    }
    waiters_head_ = &node;
    node.linked = true;
}

void CancelScope::remove_waiter(detail::WaitNode& node) noexcept {
    unlink(node);
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
        scope->wake_waiters();
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

void CancelScope::wake_waiters() noexcept {
    detail::WaitNode* node = waiters_head_;
    while (node != nullptr) {
        detail::WaitNode* next = node->next;
        unlink(*node);
        if (node->on_cancel != nullptr) {
            node->on_cancel(*node);
        }
        if (!node->scheduled && node->wake_on_cancel) {
            node->scheduled = true;
            if (node->cancelled_flag != nullptr) {
                *node->cancelled_flag = true;
            }
            if (node->waiter != nullptr) {
                detail::wake(*node->waiter);
            }
        }
        node = next;
    }
}

void detail::ScopeAccess::timeout_action(void* context) noexcept {
    static_cast<CancelScope*>(context)->cancel(detail::CancelCause::TIMEOUT);
}

void CancelScope::cancel() noexcept {
    cancel(detail::CancelCause::REQUESTED);
}

}  // namespace coro
