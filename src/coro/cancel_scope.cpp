//
// Coroutine runtime — cancel scope implementation.
//

#include "cancel_scope.h"

#include <algorithm>
#include <cassert>

#include "coro/detail/access.h"
#include "coro/cancelled.h"
#include "coro/detail/wait_node.h"

namespace coro {

CancelScope::CancelScope(CancelScope* parent, bool shielded) : parent_(parent), shielded_(shielded) {
    if (parent_ != nullptr) {
        parent_->children_.push_back(this);
    }
}

CancelScope::~CancelScope() noexcept {
    assert(waiters_head_ == nullptr && "cancel scope destroyed while a waiter is still parked");
    if (parent_ != nullptr) {
        auto& siblings = parent_->children_;
        const auto it = std::find(siblings.begin(), siblings.end(), this);
        if (it != siblings.end()) {
            siblings.erase(it);
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
    if (cause == detail::CancelCause::TIMEOUT) {
        timed_out_ = true;
    }
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    wake_subtree();
}

void CancelScope::wake_subtree() noexcept {
    detail::WaitNode* node = waiters_head_;
    while (node != nullptr) {
        detail::WaitNode* next = node->next;
        unlink(*node);
        if (node->on_cancel != nullptr) {
            node->on_cancel(*node);
        }
        if (!node->scheduled) {
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
    // A shielded child scope keeps its own (and its subtree's) cancellation
    // state untouched, so ancestor cancellation never reaches it.
    for (CancelScope* child : children_) {
        if (!child->shielded_) {
            child->wake_subtree();
        }
    }
}

void detail::ScopeAccess::timeout_action(void* context) noexcept {
    static_cast<CancelScope*>(context)->cancel(detail::CancelCause::TIMEOUT);
}

void CancelScope::cancel() noexcept {
    cancel(detail::CancelCause::REQUESTED);
}

}  // namespace coro
