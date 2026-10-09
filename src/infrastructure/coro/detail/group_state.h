// Coroutine runtime — the bookkeeping behind TaskGroup.
//
// A group keeps its children in intrusive links (no allocation on completion,
// so the completion hook can be noexcept) and hands the public TaskGroup a
// state pointer. Everything here is unreachable except through TaskGroup.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_STATE_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_GROUP_STATE_H

#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/detail/task_promise.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro::detail {

template<typename Fn, bool Supervisor>
Task<void> run_group(Fn fn);

/// Compile-time child identity, used to match `next<T>()` against a spawn.
template<typename T>
struct TypeTag {
    static constexpr int VALUE = 0;
};

/// One spawned child: its frame plus the bookkeeping the group needs.
///
/// Owned by GroupState and destroyed in reap(); borrows `frame`, which the
/// group also owns.
struct ChildSlot {
    PromiseBase* frame = nullptr;
    const void* type_tag = nullptr;
    /// Head of the intrusive list of frames parked in `co_await handle`.
    PromiseBase* join_head = nullptr;
    /// Intrusive completion FIFO link (completion order == next() order).
    ChildSlot* completed_next = nullptr;
    bool done = false;
    bool consumed = false;
    /// spawn_discard child: the slot is erased at completion instead of
    /// lingering until scope exit, and its result is never delivered.
    bool discard = false;
};

/// Everything a running group needs; lives in the group combinator's frame.
///
/// Loop-thread only. All bookkeeping is intrusive, so child completion never
/// allocates and the completion hook can be noexcept.
struct GroupState {
    GroupState(Loop* loop_ptr, CancelScope* parent, bool supervised)
        : loop(loop_ptr), supervisor(supervised), scope(parent, false) {}

    GroupState(const GroupState&) = delete;
    GroupState& operator=(const GroupState&) = delete;

    Loop* loop = nullptr;
    bool supervisor = false;
    /// Scope every child runs in; cancelling it stops the children.
    CancelScope scope;
    std::vector<std::unique_ptr<ChildSlot>> children;
    /// Completed discard child's frame, awaiting destruction. A completion hook
    /// runs inside the child's own final suspend, where destroying that frame
    /// is UB, so the hook parks it here and the next completion (or reap())
    /// destroys it — the graveyard never holds more than one frame.
    PromiseBase* graveyard = nullptr;
    ChildSlot* completed_head = nullptr;
    ChildSlot* completed_tail = nullptr;
    PromiseBase* next_head = nullptr;
    PromiseBase* done_waiter = nullptr;
    std::exception_ptr first_error;
    std::size_t done_count = 0;

    [[nodiscard]] bool all_done() const noexcept { return done_count == children.size(); }

    /// Allocates one slot; called from spawn(), which may throw.
    ChildSlot* add_child(const void* tag) {
        auto slot = std::make_unique<ChildSlot>();
        slot->type_tag = tag;
        ChildSlot* raw = slot.get();
        children.push_back(std::move(slot));
        return raw;
    }

    /// Child completion hook: runs inside the child's final suspend on the loop
    /// thread. Allocation-free by construction, hence noexcept.
    void child_finished(PromiseBase& frame) noexcept {
        ChildSlot* slot = find(frame);
        if (slot == nullptr) {
            return;
        }
        slot->done = true;
        if (frame.failed() && !first_error) {
            first_error = frame.error;
            if (!supervisor) {
                ScopeAccess::cancel(scope, CancelCause::REQUESTED);
            }
        }
        for (PromiseBase* waiter = std::exchange(slot->join_head, nullptr); waiter != nullptr;) {
            PromiseBase* next = std::exchange(waiter->wait_next, nullptr);
            wake(*waiter);
            waiter = next;
        }
        if (slot->discard) {
            // No FIFO entry and no result: the slot leaves the bookkeeping now.
            // The frame is parked in the graveyard after destroying the
            // previous occupant — this hook cannot destroy the frame it runs
            // on. done_count is untouched: the slot no longer exists.
            destroy_graveyard();
            graveyard = &frame;
            erase_slot(slot);
        } else {
            ++done_count;
            slot->completed_next = nullptr;
            if (completed_tail != nullptr) {
                completed_tail->completed_next = slot;
            } else {
                completed_head = slot;
            }
            completed_tail = slot;
        }
        for (PromiseBase* waiter = std::exchange(next_head, nullptr); waiter != nullptr;) {
            PromiseBase* next = std::exchange(waiter->wait_next, nullptr);
            wake(*waiter);
            waiter = next;
        }
        if (done_waiter != nullptr && all_done()) {
            PromiseBase* waiter = std::exchange(done_waiter, nullptr);
            wake(*waiter);
        }
    }

    /// Pop the oldest unconsumed completed child of the given result type.
    ChildSlot* take_completed(const void* tag) noexcept {
        ChildSlot* previous = nullptr;
        for (ChildSlot* slot = completed_head; slot != nullptr; slot = slot->completed_next) {
            if (slot->type_tag == tag && !slot->consumed) {
                slot->consumed = true;
                unlink_completed(previous, slot);
                return slot;
            }
            previous = slot;
        }
        return nullptr;
    }

    /// A result claimed by a join no longer belongs to next(), even if pending.
    [[nodiscard]] bool has_unconsumed(const void* tag) const noexcept {
        for (const auto& slot : children) {
            if (slot->type_tag == tag && !slot->consumed) {
                return true;
            }
        }
        return false;
    }

    /// Destroy every child frame. Children may refer to the body frame, so the
    /// body is destroyed only after this returns.
    void reap() noexcept {
        destroy_graveyard();
        for (auto& slot : children) {
            if (slot->frame != nullptr) {
                slot->frame->self.destroy();
                slot->frame = nullptr;
            }
        }
        children.clear();
        completed_head = nullptr;
        completed_tail = nullptr;
        next_head = nullptr;
        done_waiter = nullptr;
        done_count = 0;
    }

private:
    /// Destroy the graveyard frame, if one is parked. Safe from any context:
    /// the parked frame is completed and nobody references it any more.
    void destroy_graveyard() noexcept {
        if (graveyard != nullptr) {
            graveyard->self.destroy();
            graveyard = nullptr;
        }
    }

    /// Remove a slot from the bookkeeping. Vector erase only moves the
    /// unique_ptrs, so it cannot allocate; the slot must not be touched after.
    void erase_slot(ChildSlot* slot) noexcept {
        for (auto it = children.begin(); it != children.end(); ++it) {
            if (it->get() == slot) {
                children.erase(it);
                return;
            }
        }
    }

    ChildSlot* find(const PromiseBase& frame) const noexcept {
        for (const auto& candidate : children) {
            if (candidate->frame == &frame) {
                return candidate.get();
            }
        }
        return nullptr;
    }

    void unlink_completed(ChildSlot* previous, ChildSlot* slot) noexcept {
        if (previous != nullptr) {
            previous->completed_next = slot->completed_next;
        } else {
            completed_head = slot->completed_next;
        }
        if (completed_tail == slot) {
            completed_tail = previous;
        }
        slot->completed_next = nullptr;
    }
};

inline void group_child_completed(void* owner, PromiseBase& frame) noexcept {
    static_cast<GroupState*>(owner)->child_finished(frame);
}

/// Parks the awaiting frame until any child completes.
struct NextAwaitable {
    GroupState* state = nullptr;

    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& waiter = handle.promise();
        waiter.wait_next = state->next_head;
        state->next_head = &waiter;
        return true;
    }

    void await_resume() const noexcept {}
};

/// Parks the awaiting frame until every child has completed.
struct AllDoneAwaitable {
    GroupState* state = nullptr;

    bool await_ready() const noexcept { return state->all_done(); }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        state->done_waiter = &handle.promise();
        return true;
    }

    void await_resume() const noexcept {}
};

/// Awaits one child frame; used by Handle's operator co_await.
template<typename T>
struct JoinAwaiter {
    ChildSlot* slot = nullptr;

    bool await_ready() const {
        if (slot == nullptr || slot->consumed) {
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

/// Starts a not-yet-started frame and parks the caller until it finishes,
/// *without* destroying it. The group uses this so the body frame outlives the
/// children it spawned: children commonly capture the body's locals, and the
/// body is reaped together with them at scope exit.
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