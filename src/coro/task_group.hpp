//
// Coroutine runtime — the group types.
//
// TaskGroup and its Handle live in their own public header because the group
// combinator's body (coro::detail::run_group) has to construct a TaskGroup and
// therefore needs it complete, while detail/group_runner.hpp needs to include
// this header to get it. Keeping them apart lets group.hpp be pure combinators
// and detail/group_runner.hpp be self-contained; including run_group back here
// would close the cycle.
//
// See the dependency rule in coro.h.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_TASK_GROUP_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_TASK_GROUP_HPP

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>

#include <coroutine>

#include <expected>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/group_state.h"
#include "coro/detail/task_promise.h"  // IWYU pragma: export
#include "coro/fwd.h"
#include "coro/loop.h"
#include "coro/task.hpp"

namespace coro {

/// A child's outcome: its value, or the defect that aborted it.
///
/// A defect is a value here because the group reports it rather than aborting
/// the caller; only task_group rethrows it at scope exit.
template<typename T>
using Result = std::expected<T, std::exception_ptr>;

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

    /// True while the group's live slot has an unclaimed result.
    [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr && !slot_->consumed; }

    /// Join the child: yields its value or rethrows the defect that aborted it.
    /// Claims the result once, shared across Handle copies and next<T>().
    /// An empty or already claimed handle throws std::logic_error.
    /// Not a cancellation checkpoint — the join always completes, because the
    /// child belongs to the group's scope and observes cancellation there.
    detail::JoinAwaiter<T> operator co_await() noexcept { return detail::JoinAwaiter<T>{slot_}; }

private:
    friend class TaskGroup;

    explicit Handle(detail::ChildSlot* slot) noexcept : slot_(slot) {}

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
        assert(task.valid() && "spawn requires a valid task");
        detail::ChildSlot* slot = state_->add_child(&detail::TypeTag<T>::VALUE);
        auto handle = detail::TaskAccess::release(task);
        start_child(handle.promise(), *slot);
        return Handle<T>{slot};
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
        assert(task.valid() && "spawn_discard requires a valid task");
        detail::ChildSlot* slot = state_->add_child(nullptr);
        slot->discard = true;
        auto handle = detail::TaskAccess::release(task);
        start_child(handle.promise(), *slot);
    }

    /// Consume results of children whose result type is `T`, in completion
    /// order. Returns nullopt once no such child can complete any more.
    ///
    /// Cancellation: a checkpoint; the group's own scope governs the
    /// children. Failure: allocation may throw; the returned deferred defect is
    /// a value in the Result.
    template<typename T>
    [[nodiscard]] Task<std::optional<Result<T>>> next() {
        for (;;) {
            state_->scope.throw_if_cancelled();
            if (detail::ChildSlot* slot = state_->take_completed(&detail::TypeTag<T>::VALUE)) {
                auto* promise = static_cast<detail::TaskPromise<T>*>(slot->frame);
                if (promise->cancellation) {
                    continue;  // cancelled children have no result value
                }
                if (promise->failed()) {
                    co_return Result<T>{std::unexpect, promise->error};
                }
                if constexpr (std::is_void_v<T>) {
                    co_return Result<T>{};
                } else {
                    co_return Result<T>{std::move(*promise->value)};
                }
            }
            if (!state_->has_unconsumed(&detail::TypeTag<T>::VALUE)) {
                co_return std::nullopt;
            }
            co_await detail::NextAwaitable{state_};
        }
    }

    /// Cancel every child (and the group body) of this group.
    void cancel() noexcept { detail::ScopeAccess::cancel(state_->scope, detail::CancelCause::REQUESTED); }

    /// The group's child scope; borrowed for the lifetime of the group.
    [[nodiscard]] CancelScope& scope() noexcept { return state_->scope; }

private:
    template<typename Fn, bool Supervisor>
    friend Task<void> detail::run_group(Fn fn);

    explicit TaskGroup(detail::GroupState* state) noexcept : state_(state) {}

    /// Bind one not-yet-started frame to this group, allocate its slot and
    /// schedule the first resumption. Slot allocation precedes ownership transfer.
    void start_child(detail::PromiseBase& promise, detail::ChildSlot& slot) noexcept {
        promise.loop = state_->loop;
        promise.scope = &state_->scope;
        promise.context_bound = true;
        promise.completion_owner = state_;
        promise.completion = &detail::group_child_completed;
        slot.frame = &promise;
        detail::LoopAccess::schedule(*state_->loop, promise);
    }

    detail::GroupState* state_ = nullptr;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_TASK_GROUP_HPP