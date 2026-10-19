// Coroutine runtime — nursery capabilities and single-consumer child results.
#ifndef YADDNSC_CORO_NURSERY_HPP
#define YADDNSC_CORO_NURSERY_HPP

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/nursery_join.hpp"
#include "coro/detail/nursery_next_wait.hpp"  // IWYU pragma: export
#include "coro/detail/nursery_state.hpp"
#include "coro/detail/task_promise.hpp"  // IWYU pragma: export
#include "coro/fwd.h"
#include "coro/task.hpp"

namespace coro {

/// A single-consumer result capability, shared across copies and next<T>().
///
/// The nursery owns execution; this handle only retains bookkeeping. Consumption
/// requires the owning loop and a structured descendant of the nursery runner,
/// even if the result is already ready. Nursery exit invalidates all handles and
/// releases their payloads; retained copies keep empty storage only.
/// Thread safety: loop thread only, including valid() and destruction.
template<typename T>
class Handle {
public:
    Handle() = default;

    /// True while the nursery owns an unreserved, unconsumed result.
    [[nodiscard]] bool valid() const noexcept {
        return entry_ != nullptr && entry_->owner != nullptr && entry_->available();
    }

    /// Consume once after producer retirement; rethrow its defect/cancellation.
    /// Empty, expired, reserved or structurally invalid consumption throws
    /// std::logic_error. Cancellation releases the reservation without consuming
    /// the result or cancelling the producer. A throwing value move still consumes.
    detail::JoinAwaiter<T> operator co_await() noexcept { return detail::JoinAwaiter<T>{entry_}; }

private:
    friend class Nursery;

    explicit Handle(std::shared_ptr<detail::ChildEntry<T>> entry) noexcept : entry_(std::move(entry)) {}

    std::shared_ptr<detail::ChildEntry<T>> entry_;
};

namespace detail {

/// The nursery runner coroutine, defined in detail/nursery_runner.hpp. Declared
/// here because Nursery's private section below is the only place that names it.
template<typename Resources, typename Fn>
Task<void> run_nursery(std::optional<Resources> resources, std::optional<Fn> fn, NurseryPolicy policy);

}  // namespace detail

/// Borrowed concurrency capability supplied only by with_nursery().
///
/// The runner owns Resources/Fn through child retirement and result cleanup.
/// Body locals and body parameters do not survive body return: children must
/// borrow runner-owned resources or an owner covering the entire nursery await.
/// Normal body return closes admission and drains without cancelling children;
/// abnormal return cancels before draining. Thread safety: loop thread only.
class Nursery {
public:
    Nursery(const Nursery&) = delete;
    Nursery& operator=(const Nursery&) = delete;

    /// Queue a child in the nursery scope, returning its result handle immediately.
    /// Ignoring the handle is supported; scope exit still drains the child.
    /// Throws std::logic_error for a closed nursery or invalid task; preparation
    /// may throw std::bad_alloc. Failure never leaves a partially started child.
    template<typename T>
    Handle<T> spawn(Task<T> task) {
        check_admission(task.valid());
        auto entry = state_->prepare_child<T>(&detail::TypeTag<T>::VALUE);
        start_nursery_child(task, *entry);
        return Handle<T>{std::move(entry)};
    }

    /// Queue a void child without a consumable handle. Defects still cancel
    /// siblings under FAIL_FAST or become unobserved errors under SUPERVISOR.
    /// Admission/allocation failures are the same as spawn().
    void spawn_discard(Task<void> task) {
        check_admission(task.valid());
        auto entry = state_->prepare_child<void>(nullptr);
        entry->discard = true;
        start_nursery_child(task, *entry);
    }

    /// Consume the oldest safely published unreserved result of type T.
    /// Reservations do not change publication order. A parked call keeps waiting
    /// while a producer of T is still running and while a *published* result of T
    /// is merely reserved: that reservation can be handed back, which wakes these
    /// waiters again. `nullopt` therefore means the queue holds no claimable result
    /// of T and nothing this nursery already produced can become claimable again;
    /// later spawns still reach later calls. A still-running child whose pending
    /// outcome a Handle reserved is not waited on. Requires the owning loop and a
    /// runner descendant. Cancellation precedes consumption and never cancels
    /// producers. Child defects are Result errors; invalid context throws
    /// std::logic_error; value movement may throw. Cancelled children are skipped.
    ///
    /// The returned awaitable parks the caller's own execution: a call never
    /// starts a nested execution the way a Task-returning wrapper would.
    template<typename T>
    [[nodiscard]] detail::NextAwaitable<T> next() {
        return detail::NextAwaitable<T>{*state_};
    }

    /// Request cancellation of the nursery body and children; does not drain.
    void cancel() noexcept { detail::ScopeAccess::cancel(state_->scope, detail::CancelCause::REQUESTED); }

    /// Borrow the nursery scope until with_nursery finishes.
    [[nodiscard]] CancelScope& scope() noexcept { return state_->scope; }

private:
    template<typename Resources, typename Fn>
    friend Task<void> detail::run_nursery(std::optional<Resources>, std::optional<Fn>, NurseryPolicy);
    friend struct detail::NurseryAccess;

    explicit Nursery(detail::NurseryState* state) noexcept : state_(state) { state_->lifetime->nursery = this; }

    void check_admission(bool valid_task) const {
        if (!state_->accepting || !valid_task) {
            throw std::logic_error("spawn requires an open nursery and a valid task");
        }
    }

    /// No-throw activation commit after all entry preparation has succeeded.
    template<typename T>
    void start_nursery_child(Task<T>& task, detail::ChildEntry<T>& entry) noexcept {
        const auto handle = detail::TaskAccess::release(task);
        detail::TaskPromise<T>& promise = handle.promise();
        promise.result = &entry.typed_result;
        detail::Activation::commit<&detail::nursery_child_terminal, &detail::nursery_child_retired>(
            promise, entry.record, entry.typed_result, *state_->loop, &state_->scope, state_->runner, &entry);
        entry.record.adopt(handle);
        entry.record.mark_active();
        detail::LoopAccess::schedule(*state_->loop, entry.record);
    }

    detail::NurseryState* state_;
};

inline std::shared_ptr<detail::NurseryLifetime> detail::NurseryAccess::lifetime(Nursery& nursery) noexcept {
    return nursery.state_->lifetime;
}

}  // namespace coro

#endif  // YADDNSC_CORO_NURSERY_HPP
