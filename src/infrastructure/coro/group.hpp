//
// Coroutine runtime — structured concurrency.
//
// Two scopes with complete semantics:
//   task_group        first child failure cancels the siblings, the first
//                     defect propagates after every child has been joined
//   supervisor_group  a child failure is only reported; siblings are untouched
//
// `spawn` returns a Handle; `co_await handle` joins that child. `next()`
// consumes child results in completion order. `spawn_discard` starts a
// fire-and-forget child whose slot leaves the bookkeeping the moment it
// completes, so a group that lives for the whole process stays bounded by its
// live children instead of its history. Scope exit always joins every child
// and reaps its frame — there are no detached tasks.
//
// The body frame stays alive until every child it spawned has been reaped, so a
// child may refer to the body's locals.
//

#ifndef YADDNSC_CORO_GROUP_HPP
#define YADDNSC_CORO_GROUP_HPP

#include <cassert>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/task.hpp"

namespace coro {

/// A child's outcome: its value, or the defect that aborted it.
///
/// A defect is a value here because the group reports it rather than aborting
/// the caller; only task_group rethrows it at scope exit.
template<typename T>
using Result = std::expected<T, std::exception_ptr>;

namespace detail {

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
                scope.cancel(CancelCause::REQUESTED);
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

    bool await_ready() const noexcept { return false; }

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

    bool await_ready() const noexcept { return slot != nullptr && slot->done; }

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

}  // namespace detail

/// A joiner for a spawned child.
///
/// Ownership: non-owning. The group owns the child frame and destroys it at
/// scope exit, so a Handle must not outlive its group, and must not be used
/// after the group's `next<T>()`/join cycle has consumed the child.
/// Failure: `co_await handle` rethrows the defect that aborted the child.
template<typename T>
class Handle {
public:
    Handle() = default;

    explicit Handle(detail::ChildSlot* slot) noexcept : slot_(slot) {}

    /// True while the handle still refers to a live child slot.
    [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }

    /// Join the child: yields its value or rethrows the defect that aborted it.
    /// Not a cancellation checkpoint — the join always completes, because the
    /// child belongs to the group's scope and observes cancellation there.
    detail::JoinAwaiter<T> operator co_await() noexcept { return detail::JoinAwaiter<T>{slot_}; }

private:
    detail::ChildSlot* slot_ = nullptr;
};

/// The group handle passed to a task_group / supervisor_group body.
///
/// Ownership: borrows; construct only through `task_group`/`supervisor_group`.
/// A spawned child's frame is owned by the group until scope exit; a discard
/// child's only until the next completion after its own.
/// Thread safety: loop thread only; spawn/next/cancel are not reentrant.
class TaskGroup {
public:
    explicit TaskGroup(detail::GroupState* state) noexcept : state_(state) {}

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    /// Start a child inside the group's scope; the group owns its frame.
    ///
    /// The child starts suspended and resumes through the ready queue. Not
    /// `[[nodiscard]]` on purpose: "spawned and not explicitly joined" is a
    /// supported shape — the group joins it at scope exit.
    /// Failure: allocates; may throw std::bad_alloc.
    template<typename T>
    Handle<T> spawn(Task<T> task) {
        auto handle = task.release();
        assert(handle && "spawn requires a valid task");
        return Handle<T>{start_child(handle.promise(), &detail::TypeTag<T>::VALUE)};
    }

    /// Start a fire-and-forget child whose result is discarded.
    ///
    /// Unlike spawn(), a completed discard child leaves the bookkeeping at
    /// once: its slot is erased when it finishes and its frame is destroyed by
    /// the next completion or at scope exit, so a group that lives for the
    /// whole process (the plugin bridge's) stays bounded by its live children
    /// instead of accumulating every completed frame until shutdown. Scope exit
    /// still joins a discard child that is in flight. A defect lands in the
    /// group's first_error like any child's (and cancels a task_group's
    /// siblings).
    /// Failure: allocates; may throw std::bad_alloc.
    void spawn_discard(Task<void> task) {
        auto handle = task.release();
        assert(handle && "spawn_discard requires a valid task");
        detail::ChildSlot* slot = start_child(handle.promise(), nullptr);
        slot->discard = true;
    }

    /// Consume results of children whose result type is `T`, in completion
    /// order. Returns nullopt once no such child can complete any more.
    ///
    /// Cancellation: not a checkpoint; the group's own scope governs the
    /// children. Failure: allocation may throw; the returned deferred defect is
    /// a value in the Result.
    template<typename T>
    [[nodiscard]] Task<std::optional<Result<T>>> next() {
        for (;;) {
            if (detail::ChildSlot* slot = state_->take_completed(&detail::TypeTag<T>::VALUE)) {
                auto* promise = static_cast<TaskPromise<T>*>(slot->frame);
                if (promise->failed()) {
                    co_return Result<T>{std::unexpect, promise->error};
                }
                if constexpr (std::is_void_v<T>) {
                    co_return Result<T>{};
                } else {
                    co_return Result<T>{std::move(*promise->value)};
                }
            }
            if (state_->all_done()) {
                co_return std::nullopt;
            }
            co_await detail::NextAwaitable{state_};
        }
    }

    /// Cancel every child (and the group body) of this group.
    void cancel() noexcept { state_->scope.cancel(CancelCause::REQUESTED); }

    /// The group's child scope; borrows the group's state.
    [[nodiscard]] CancelScope& scope() noexcept { return state_->scope; }

private:
    /// Bind one not-yet-started frame to this group, allocate its slot and
    /// schedule the first resumption. Allocates; may throw std::bad_alloc.
    detail::ChildSlot* start_child(PromiseBase& promise, const void* tag) {
        detail::ChildSlot* slot = state_->add_child(tag);
        promise.loop = state_->loop;
        promise.scope = &state_->scope;
        promise.context_bound = true;
        promise.completion_owner = state_;
        promise.completion = &detail::group_child_completed;
        slot->frame = &promise;
        state_->loop->schedule(promise);
        return slot;
    }

    detail::GroupState* state_ = nullptr;
};

namespace detail {

/// Starts a not-yet-started frame and parks the caller until it finishes,
/// *without* destroying it. The group uses this so the body frame outlives the
/// children it spawned: children commonly capture the body's locals, and the
/// body is reaped together with them at scope exit.
struct StartAndAwait {
    PromiseBase* frame = nullptr;

    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> parent) noexcept {
        frame->continuation = &parent.promise();
        frame->loop->schedule(*frame);
    }

    void await_resume() const noexcept {}
};

template<typename Fn, bool Supervisor>
Task<void> run_group(Fn fn) {
    const Context context = co_await GetContext{};
    GroupState state{context.loop, context.scope, Supervisor};
    TaskGroup group{&state};

    Task<void> body = fn(group);
    body.bind_context(*context.loop, state.scope);
    auto body_handle = body.release();
    co_await StartAndAwait{&body_handle.promise()};
    const std::exception_ptr body_error = body_handle.promise().error;

    if (body_error) {
        state.scope.cancel(CancelCause::REQUESTED);
    }
    while (!state.all_done()) {
        co_await AllDoneAwaitable{&state};
    }
    const std::exception_ptr child_error = state.first_error;
    state.reap();
    // The body frame stays alive until every child it spawned has been reaped.
    body_handle.destroy();

    if (body_error) {
        std::rethrow_exception(body_error);
    }
    if constexpr (!Supervisor) {
        if (child_error) {
            std::rethrow_exception(child_error);
        }
    }
    co_return;
}

}  // namespace detail

/// Structured group: a child failure cancels its siblings, then propagates.
///
/// `fn` receives the group by reference and returns the group body. The body
/// runs in the group's scope; scope exit joins every child and reaps every
/// frame. The first child defect is rethrown after all children are joined.
/// Thread safety: loop thread only. Failure: allocation may throw; the group
/// task may complete with a child's or the body's defect.
template<typename Fn>
[[nodiscard]] Task<void> task_group(Fn fn) {
    return detail::run_group<Fn, false>(std::move(fn));
}

/// Supervised group: a child failure is reported through next(), siblings run on.
///
/// Same structure and ownership as task_group; only the child-failure policy
/// differs (a child defect is stored in its Result and never cancels siblings).
template<typename Fn>
[[nodiscard]] Task<void> supervisor_group(Fn fn) {
    return detail::run_group<Fn, true>(std::move(fn));
}

}  // namespace coro

#endif  // YADDNSC_CORO_GROUP_HPP
