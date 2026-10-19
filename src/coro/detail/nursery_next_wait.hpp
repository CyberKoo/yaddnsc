// Coroutine runtime — the frame-free consumer of Nursery::next<T>().
//
// Lives beside the other awaiters: the awaiter a public header awaits with is a
// detail type, and nursery.hpp includes it rather than inlining it.
#ifndef YADDNSC_CORO_DETAIL_NURSERY_NEXT_WAIT_HPP
#define YADDNSC_CORO_DETAIL_NURSERY_NEXT_WAIT_HPP

#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>

#include "coro/cancel_scope.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/nursery_claims.hpp"
#include "coro/detail/nursery_state.hpp"
#include "coro/detail/promise_base.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/wait_operation.h"
#include "coro/fwd.h"

namespace coro::detail {

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
    static bool satisfy_thunk(NextWaiter& waiter) noexcept { return static_cast<NextAwaitable&>(waiter).satisfy(); }

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

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_NURSERY_NEXT_WAIT_HPP
