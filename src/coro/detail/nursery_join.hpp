// Coroutine runtime — the single-consumer result reservation behind Handle.
//
// Lives beside the other awaiters: the awaiter a public header awaits with is a
// detail type, and nursery.hpp includes it rather than inlining it. It needs
// NurseryState complete — the join path validates its consumer and wakes the
// next<T>() list — so it sits above nursery_state.hpp, never below it.
#ifndef YADDNSC_CORO_DETAIL_NURSERY_JOIN_HPP
#define YADDNSC_CORO_DETAIL_NURSERY_JOIN_HPP

#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/detail/nursery_state.hpp"
#include "coro/detail/promise_base.h"
#include "coro/detail/wait_operation.h"

namespace coro::detail {

/// A single-consumer result reservation, cancellable independently of its producer.
template<typename T>
struct JoinAwaiter {
    explicit JoinAwaiter(std::shared_ptr<ChildEntry<T>> claimed) noexcept : entry(std::move(claimed)) {}

    JoinAwaiter(const JoinAwaiter&) = delete;
    JoinAwaiter& operator=(const JoinAwaiter&) = delete;

    // Started frames are reaped by the loop, never destroyed while parked. Do
    // not release the reservation here: a waiter that is still armed or still
    // holding the result would refer to this object.
    ~JoinAwaiter() noexcept {
        assert(!reserved && !wait_.subscribed() && "destroying an active Handle result wait");
        if (reserved || wait_.subscribed()) {
            std::terminate();
        }
    }

    std::shared_ptr<ChildEntry<T>> entry;
    CancelScope* scope_ = nullptr;
    CancelSubscription subscription_{};
    WaitOperation wait_{subscription_};
    bool reserved = false;

    constexpr bool await_ready() const noexcept { return false; }

    /// Reserves the result for this wait and parks unless the reap transaction
    /// already published it. Cancelling the wait releases the reservation; it
    /// never consumes the result and never cancels the producer.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& waiter = handle.promise();
        scope_ = waiter.exec->scope();
        if (scope_ != nullptr) {
            scope_->throw_if_cancelled();
        }
        if (entry == nullptr || entry->owner == nullptr || !entry->available()) {
            throw std::logic_error("Handle requires an unclaimed child result");
        }
        entry->owner->validate_consumer(*waiter.exec);
        if (!entry->result()->reserve()) {
            throw std::logic_error("Handle result is already reserved");
        }
        reserved = true;
        // A reservation change may release a next<T>() waiter that is looking for
        // this very child.
        entry->owner->wake_next(entry->type_tag);
        if (entry->result()->ready()) {
            return false;
        }
        wait_.begin(*waiter.exec);
        entry->join_waiter = &wait_;
        if (scope_ != nullptr) {
            wait_.subscribe_cancel(*scope_, this, &JoinAwaiter<T>::on_cancel);
        }
        wait_.commit_suspend();
        return true;
    }

    /// Hand the reservation back so another consumer may still claim the result.
    void release() noexcept {
        if (!reserved) {
            return;
        }
        if (entry->join_waiter == &wait_) {
            entry->join_waiter = nullptr;
        }
        entry->result()->release_reservation();
        reserved = false;
        if (entry->owner != nullptr) {
            entry->owner->wake_next(entry->type_tag);
        }
    }

    T await_resume() {
        wait_.begin_resume();
        wait_.finish();
        if (scope_ != nullptr && scope_->cancelled()) {
            release();
            scope_->throw_if_cancelled();
        }
        ResultState<T>& outcome = entry->outcome();
        // Consumption is committed before the payload moves: a throwing move
        // still ends the consumption, so a half-moved value is never retried.
        outcome.commit_consumed();
        entry->join_waiter = nullptr;
        reserved = false;
        const ConsumedEntry consume{*entry};
        if (outcome.content() != ResultContent::VALUE) {
            std::rethrow_exception(outcome.error());
        }
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return outcome.take_value();
        }
    }

private:
    /// A cancelled wait gives the reservation back and resumes; the producer is
    /// untouched, and the result stays consumable.
    static CancelAction on_cancel(void* context) noexcept {
        static_cast<JoinAwaiter<T>*>(context)->release();
        return CancelAction::RESUME;
    }
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_NURSERY_JOIN_HPP
