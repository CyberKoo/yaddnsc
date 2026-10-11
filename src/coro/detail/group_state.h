// Coroutine runtime — scope-owned children, result claims and completion waits.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_STATE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_STATE_H

#include <cassert>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <coroutine>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/task_promise.h"
#include "coro/detail/wait_node.h"
#include "coro/fwd.h"
#include "coro/loop.h"

namespace coro::detail {

template<typename Fn, bool Supervisor>
Task<void> run_group(Fn fn);

template<typename T>
struct TypeTag {
    static constexpr int VALUE = 0;
};

struct GroupState;
struct NextAwaitable;

/// Shared only to diagnose stale Handles; execution and results remain scope-owned.
struct ChildSlot {
    GroupState* owner = nullptr;
    std::size_t index = 0;
    PromiseBase* frame = nullptr;
    const void* type_tag = nullptr;
    PromiseBase* join_head = nullptr;
    ChildSlot* completed_next = nullptr;
    ChildSlot* completed_prev = nullptr;
    bool in_completed = false;
    bool done = false;
    bool consumed = false;
    bool discard = false;
};

/// Loop-thread lifetime token. A posted infrastructure callback must check this
/// token on the loop, never retain a raw TaskGroup beyond its body lifetime.
struct GroupLifetime {
    TaskGroup* group = nullptr;
};

/// Infrastructure-only ingress token; read exclusively by loop callbacks.
struct GroupAccess {
    static std::shared_ptr<GroupLifetime> lifetime(TaskGroup& group) noexcept;
};

struct GroupState {
    GroupState(Loop* loop_ptr, CancelScope* parent, bool supervised)
        : loop(loop_ptr), supervisor(supervised), scope(parent, false) {}

    GroupState(const GroupState&) = delete;
    GroupState& operator=(const GroupState&) = delete;

    ~GroupState() noexcept {
        assert(all_done());
        reap();
    }

    Loop* loop = nullptr;
    bool supervisor = false;
    CancelScope scope;
    std::vector<std::shared_ptr<ChildSlot>> children;
    std::shared_ptr<GroupLifetime> lifetime = std::make_shared<GroupLifetime>();
    ChildSlot* completed_head = nullptr;
    ChildSlot* completed_tail = nullptr;
    NextAwaitable* next_head = nullptr;
    PromiseBase* done_waiter = nullptr;
    std::exception_ptr first_error;
    std::size_t active_count = 0;

    [[nodiscard]] bool all_done() const noexcept { return active_count == 0; }

    std::shared_ptr<ChildSlot> add_child(const void* tag) {
        auto slot = std::make_shared<ChildSlot>();
        slot->owner = this;
        slot->index = children.size();
        slot->type_tag = tag;
        children.push_back(slot);
        ++active_count;
        return slot;
    }

    void child_finished(ChildSlot& slot, PromiseBase& frame) noexcept;
    void wake_next() noexcept;

    void unlink_completed(ChildSlot& slot) noexcept {
        if (!slot.in_completed) {
            return;
        }
        if (slot.completed_prev != nullptr) {
            slot.completed_prev->completed_next = slot.completed_next;
        } else {
            completed_head = slot.completed_next;
        }
        if (slot.completed_next != nullptr) {
            slot.completed_next->completed_prev = slot.completed_prev;
        } else {
            completed_tail = slot.completed_prev;
        }
        slot.completed_next = nullptr;
        slot.completed_prev = nullptr;
        slot.in_completed = false;
    }

    /// Claim and unlink, but keep the frame alive through result extraction.
    ChildSlot* take_completed(const void* tag) noexcept {
        for (ChildSlot* slot = completed_head; slot != nullptr; slot = slot->completed_next) {
            if (slot->type_tag == tag && !slot->consumed) {
                slot->consumed = true;
                unlink_completed(*slot);
                return slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has_unconsumed(const void* tag) const noexcept {
        for (const auto& slot : children) {
            if (!slot->discard && slot->type_tag == tag && !slot->consumed) {
                return true;
            }
        }
        return false;
    }

    /// Consumption is terminal even if moving the value throws. This is called
    /// only after completion, outside the child's final_suspend.
    void retire(ChildSlot& slot) noexcept {
        assert(slot.done);
        unlink_completed(slot);
        PromiseBase* frame = std::exchange(slot.frame, nullptr);
        slot.owner = nullptr;
        erase_slot(slot);
        frame->self.destroy();
    }

    void erase_slot(ChildSlot& slot) noexcept {
        const std::size_t index = slot.index;
        if (index + 1 != children.size()) {
            children[index] = std::move(children.back());
            children[index]->index = index;
        }
        children.pop_back();
    }

    /// Only unobserved supervised failures remain here: observed results retire
    /// their slots; failed discard children are deliberately retained for exit.
    [[nodiscard]] std::vector<std::exception_ptr> unobserved_errors() const {
        std::vector<std::exception_ptr> errors;
        for (const auto& slot : children) {
            if (slot->frame->failed()) {
                errors.push_back(slot->frame->error);
            }
        }
        return errors;
    }

    void reap() noexcept {
        lifetime->group = nullptr;
        for (auto& slot : children) {
            slot->frame->self.destroy();
            slot->frame = nullptr;
            slot->owner = nullptr;
        }
        children.clear();
        completed_head = nullptr;
        completed_tail = nullptr;
        next_head = nullptr;
        done_waiter = nullptr;
    }
};

/// A waiter belongs to its caller's scope, not to the result producer's scope.
/// Cancellation detaches just this subscription; no child result is claimed.
struct NextAwaitable {
    explicit NextAwaitable(GroupState* group) noexcept : state(group) {}

    NextAwaitable(const NextAwaitable&) = delete;
    NextAwaitable& operator=(const NextAwaitable&) = delete;
    GroupState* state;
    NextAwaitable* next = nullptr;
    NextAwaitable* prev = nullptr;
    WaitNode node{};
    bool linked = false;

    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& waiter = handle.promise();
        node.scope = waiter.scope;
        if (node.scope != nullptr) {
            node.scope->throw_if_cancelled();
        }
        node.waiter = &waiter;
        node.owner = this;
        node.on_cancel = [](WaitNode& cancelled) noexcept { static_cast<NextAwaitable*>(cancelled.owner)->detach(); };
        next = state->next_head;
        if (next != nullptr) {
            next->prev = this;
        }
        state->next_head = this;
        linked = true;
        if (node.scope != nullptr) {
            ScopeAccess::add_waiter(*node.scope, node);
        }
        return true;
    }

    void detach() noexcept {
        if (!linked) {
            return;
        }
        if (prev != nullptr) {
            prev->next = next;
        } else {
            state->next_head = next;
        }
        if (next != nullptr) {
            next->prev = prev;
        }
        next = nullptr;
        prev = nullptr;
        linked = false;
    }

    void resolve() noexcept {
        detach();
        if (node.linked) {
            ScopeAccess::remove_waiter(*node.scope, node);
        }
        if (!node.scheduled) {
            node.scheduled = true;
            wake(*node.waiter);
        }
    }

    void await_resume() const {
        if (node.scope != nullptr) {
            node.scope->throw_if_cancelled();
        }
    }
};

inline void GroupState::wake_next() noexcept {
    while (next_head != nullptr) {
        next_head->resolve();
    }
}

inline void GroupState::child_finished(ChildSlot& slot, PromiseBase& frame) noexcept {
    slot.done = true;
    --active_count;
    if (frame.failed() && !supervisor && !first_error) {
        first_error = frame.error;
        ScopeAccess::cancel(scope, CancelCause::REQUESTED);
    }
    for (PromiseBase* waiter = std::exchange(slot.join_head, nullptr); waiter != nullptr;) {
        PromiseBase* next = std::exchange(waiter->wait_next, nullptr);
        wake(*waiter);
        waiter = next;
    }
    if (slot.discard && !frame.failed()) {
        // Reaping belongs to the loop's safe phase, not another child's hook.
        slot.frame = nullptr;
        slot.owner = nullptr;
        erase_slot(slot);
        LoopAccess::defer_reap(*loop, frame);
    } else if (!slot.discard) {
        slot.completed_prev = completed_tail;
        slot.in_completed = true;
        if (completed_tail != nullptr) {
            completed_tail->completed_next = &slot;
        } else {
            completed_head = &slot;
        }
        completed_tail = &slot;
    }
    wake_next();
    if (done_waiter != nullptr && all_done()) {
        wake(*std::exchange(done_waiter, nullptr));
    }
}

inline void group_child_completed(void* owner, PromiseBase& frame) noexcept {
    auto& slot = *static_cast<ChildSlot*>(owner);
    slot.owner->child_finished(slot, frame);
}

/// Exception-safe consumption: frame and history retire even on a throwing move.
struct ConsumedChild {
    ChildSlot& slot;

    ~ConsumedChild() noexcept { slot.owner->retire(slot); }
};

/// Internal drain is intentionally not a cancellation checkpoint. Children must
/// finish before their owner or borrowed outer resources can be destroyed.
struct AllDoneAwaitable {
    GroupState* state = nullptr;

    bool await_ready() const noexcept { return state->all_done(); }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if (state->done_waiter != nullptr) {
            throw std::logic_error("only one group drain may be active");
        }
        state->done_waiter = &handle.promise();
        return true;
    }

    void await_resume() const noexcept {}
};

/// Handle join retains its existing non-interruptible, single-consumer contract.
template<typename T>
struct JoinAwaiter {
    std::shared_ptr<ChildSlot> slot;

    bool await_ready() const {
        if (slot == nullptr || slot->frame == nullptr || slot->consumed) {
            throw std::logic_error("join requires an unclaimed child result");
        }
        slot->consumed = true;
        return slot->done;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& waiter = handle.promise();
        waiter.wait_next = slot->join_head;
        slot->join_head = &waiter;
        return true;
    }

    T await_resume() {
        if (slot->frame == nullptr) {
            throw std::logic_error("join requires an unclaimed child result");
        }
        const ConsumedChild consume{*slot};
        auto* promise = static_cast<TaskPromise<T>*>(slot->frame);
        if (promise->error) {
            std::rethrow_exception(promise->error);
        }
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(*promise->value);
        }
    }
};

/// Keep the body frame (not already-destroyed body locals) until owner exit.
struct StartAndAwait {
    PromiseBase* frame = nullptr;

    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) noexcept {
        frame->continuation = &parent.promise();
        LoopAccess::schedule(*frame->loop, *frame);
    }

    void await_resume() const noexcept {}
};

}  // namespace coro::detail
#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_STATE_H
