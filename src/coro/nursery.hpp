// Coroutine runtime — nursery capabilities and single-consumer child results.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_NURSERY_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_NURSERY_HPP

#include <cassert>
#include <coroutine>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/context_awaitables.h"
#include "coro/detail/nursery_state.hpp"
#include "coro/detail/task_promise.hpp"  // IWYU pragma: export
#include "coro/fwd.h"
#include "coro/task.hpp"

namespace coro {

/// A child's value or defect. Cancellation is not a Result error.
template<typename T>
using Result = std::expected<T, std::exception_ptr>;

namespace detail {

/// The frame-free consumer of Nursery::next<T>().
///
/// It parks the caller's own execution record instead of starting a nested
/// Task, and claims a matching result when the nursery publishes one. A waiter
/// is resumed only when it can return a result or when no unreserved producer
/// of T remains, so await_resume never has to re-park.
template<typename T>
class NextAwaitable final : public NextWaiter {
public:
    explicit NextAwaitable(NurseryState& nursery) noexcept : state_(&nursery) {
        type_tag = &TypeTag<T>::VALUE;
        try_satisfy = &NextAwaitable::satisfy_thunk;
    }

    NextAwaitable(const NextAwaitable&) = delete;
    NextAwaitable& operator=(const NextAwaitable&) = delete;
    NextAwaitable(NextAwaitable&&) = delete;
    NextAwaitable& operator=(NextAwaitable&&) = delete;

    ~NextAwaitable() noexcept {
        assert(!linked && !wait_.subscribed() && "destroying an active next<T>() wait");
        if (linked || wait_.subscribed()) {
            std::terminate();  // the nursery would keep a waiter pointer into freed storage
        }
    }

    /// Defer the context check and READY fast-path decision to await_suspend,
    /// where the caller's execution record is available.
    constexpr bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& waiter = handle.promise();
        ExecutionRecord* record = waiter.exec;
        assert(record != nullptr && "next<T>() requires a started caller");
        if (record == nullptr) {
            std::terminate();
        }
        state_->validate_consumer(*record);
        scope_ = record->scope();
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        // An already-retired result (or a type that can no longer produce one)
        // must not add a queue round trip: the design keeps the READY fast path
        // for both Handle and next. The caller resumes inline in this resume.
        if (try_claim_or_exhaust()) {
            return false;
        }
        wait_.begin(*record);
        state_->add_next_waiter(*this);
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &NextAwaitable::on_cancel);
        }
        wait_.commit_suspend();
        return true;
    }

    std::optional<Result<T>> await_resume() {
        wait_.begin_resume();
        wait_.finish();
        state_->remove_next_waiter(*this);
        if (scope_ != nullptr && scope_->cancelled()) {
            release_claim();
            scope_->throw_if_cancelled();
        }
        if (exhausted_) {
            return std::nullopt;
        }
        assert(claimed_ != nullptr && "next<T>() resumed without a result");
        if (claimed_ == nullptr) {
            std::terminate();  // a spurious wake would violate the wait protocol
        }
        ChildEntry<T>& entry = *claimed_;
        claimed_ = nullptr;
        ResultState<T>& outcome = entry.outcome();
        outcome.commit_consumed();
        const ConsumedEntry consume{entry};
        assert(outcome.content() != ResultContent::CANCELLED && "cancelled child reached next<T>()");
        if (outcome.content() == ResultContent::CANCELLED) {
            std::terminate();
        }
        if (outcome.content() == ResultContent::ERROR) {
            return Result<T>{std::unexpect, outcome.error()};
        }
        if constexpr (std::is_void_v<T>) {
            return Result<T>{};
        } else {
            return Result<T>{outcome.take_value()};
        }
    }

private:
    static bool satisfy_thunk(NextWaiter& waiter) noexcept {
        return static_cast<NextAwaitable&>(waiter).satisfy();
    }

    /// Claim a matching result or decide exhaustion without touching the wait.
    /// Used both for the inline READY fast path and from a parked wake-up.
    bool try_claim_or_exhaust() noexcept {
        if (ChildEntryBase* claimed = state_->claim_completed(type_tag)) {
            claimed_ = static_cast<ChildEntry<T>*>(claimed);
            return true;
        }
        if (!state_->has_pending(type_tag)) {
            exhausted_ = true;
            return true;
        }
        return false;
    }

    /// Claim a matching result, or report exhaustion when no producer of this
    /// type can publish one any more. False keeps the waiter parked.
    bool satisfy() noexcept {
        if (!try_claim_or_exhaust()) {
            return false;
        }
        state_->remove_next_waiter(*this);
        wait_.event_ready();
        return true;
    }

    void release_claim() noexcept {
        if (claimed_ != nullptr) {
            state_->release_claim(*claimed_);
            claimed_ = nullptr;
        }
    }

    /// Cancellation: leave the waiter chain, return any claimed reservation and
    /// take the resume so the caller rethrows at await_resume.
    static CancelAction on_cancel(void* context) noexcept {
        auto& self = *static_cast<NextAwaitable*>(context);
        self.state_->remove_next_waiter(self);
        self.release_claim();
        return CancelAction::RESUME;
    }

    NurseryState* state_;
    ChildEntry<T>* claimed_ = nullptr;
    bool exhausted_ = false;
    CancelScope* scope_ = nullptr;
    CancelSubscription subscription_{};
    WaitOperation wait_{subscription_};
};

}  // namespace detail

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

#endif  // YADDNSC_INFRASTRUCTURE_CORO_NURSERY_HPP
