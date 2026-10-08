//
// Coroutine runtime — cancel scope implementation.
//

#include "cancel_scope.h"

#include <algorithm>
#include <cassert>

#include "infrastructure/coro/loop.h"

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

void CancelScope::unlink(WaitNode& node) noexcept {
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

void CancelScope::add_waiter(WaitNode& node) noexcept {
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

void CancelScope::remove_waiter(WaitNode& node) noexcept {
    unlink(node);
}

void CancelScope::cancel(CancelCause cause) noexcept {
    if (cause == CancelCause::TIMEOUT) {
        timed_out_ = true;
    }
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    wake_subtree();
}

void CancelScope::wake_subtree() noexcept {
    WaitNode* node = waiters_head_;
    while (node != nullptr) {
        WaitNode* next = node->next;
        unlink(*node);
        if (node->on_cancel != nullptr) {
            node->on_cancel(*node);
        }
        if (!node->scheduled) {
            node->scheduled = true;
            if (node->cancelled_flag != nullptr) {
                *node->cancelled_flag = true;
            }
            if (node->waiter != nullptr && node->waiter->loop != nullptr) {
                node->waiter->loop->schedule(*node->waiter);
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

void CancelScope::timeout_action(void* scope) noexcept {
    static_cast<CancelScope*>(scope)->cancel(CancelCause::TIMEOUT);
}

}  // namespace coro
