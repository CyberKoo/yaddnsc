// Coroutine runtime — scope-owned children, result claims and completion waits.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_STATE_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_STATE_HPP

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/task_promise.hpp"
#include "coro/detail/wait_operation.h"
#include "coro/fwd.h"
#include "coro/loop.h"

namespace coro::detail {

template<typename Resources, typename Fn>
Task<void> run_nursery(std::optional<Resources> resources, std::optional<Fn> fn, NurseryPolicy policy);

template<typename T>
struct TypeTag {
    static constexpr int VALUE = 0;
};

struct NurseryState;
struct ChildEntryBase;
struct NextWaiter;

/// One nursery child: its execution record and its result slot share a single
/// allocation, so the frame's owner and its outcome live and die together.
///
/// The record is the only thing that owns a started frame; the entry only
/// borrows a reference to it. Consumption state lives in the result, not in the
/// entry, so a Handle and the nursery's completion queue always agree on who owns
/// the outcome.
struct ChildEntryBase {
    ChildEntryBase() = default;

    ChildEntryBase(const ChildEntryBase&) = delete;
    ChildEntryBase& operator=(const ChildEntryBase&) = delete;

    /// Execution record of this child; its registry and queue memberships are
    /// cleared by the loop's reap transaction before this entry may go away.
    ExecutionRecord record;

    /// The nursery, or null once the entry was consumed or the nursery exited.
    NurseryState* owner = nullptr;
    void (*clear_payload)(ChildEntryBase&) noexcept = nullptr;
    std::size_t index = 0;
    /// Type identity for next<T>(); null for a discarded child.
    const void* type_tag = nullptr;
    /// The wait parked on this child's result, if any. Owned by the awaiting
    /// frame; the entry only borrows it between reservation and resume.
    WaitOperation* join_waiter = nullptr;
    /// Completion-queue membership, in retirement order.
    ChildEntryBase* completed_next = nullptr;
    ChildEntryBase* completed_prev = nullptr;
    bool in_completed = false;
    bool discard = false;

    [[nodiscard]] ResultStateBase* result() const noexcept { return record.result(); }

    /// True while nobody owns the outcome: not reserved, consumed or expired.
    [[nodiscard]] bool available() const noexcept { return result()->consumption() == ResultConsumption::AVAILABLE; }

    /// True for a recorded defect. Cancellation is control flow, not a defect.
    [[nodiscard]] bool failed() const noexcept { return result()->content() == ResultContent::ERROR; }
};

/// A typed child entry: the untyped bookkeeping plus the result slot for `T`.
template<typename T>
struct ChildEntry final : ChildEntryBase {
    ChildEntry() noexcept {
        clear_payload = [](ChildEntryBase& entry) noexcept {
            static_cast<ChildEntry&>(entry).typed_result.clear_payload();
        };
    }

    ~ChildEntry() noexcept { record.validate_host_lifetime(); }

    ResultState<T> typed_result;

    /// The typed slot this entry was activated with; the record holds the same
    /// object through its base pointer.
    [[nodiscard]] ResultState<T>& outcome() noexcept { return typed_result; }
};

/// One parked next<T>() waiter. The typed awaitable derives from this base so
/// the nursery can satisfy a mixed-type waiter list without templates.
struct NextWaiter {
    NextWaiter() = default;

    NextWaiter(const NextWaiter&) = delete;
    NextWaiter& operator=(const NextWaiter&) = delete;
    NextWaiter(NextWaiter&&) = delete;
    NextWaiter& operator=(NextWaiter&&) = delete;

    NextWaiter* next = nullptr;
    NextWaiter* prev = nullptr;
    bool linked = false;
    const void* type_tag = nullptr;
    /// Claims a matching result or reports exhaustion. False means no claimable
    /// matching result remains but one may still become claimable — a live producer
    /// will publish, or a reserved result can be handed back — so without another
    /// event, later waiters of the same type cannot be satisfied either.
    bool (*try_satisfy)(NextWaiter&) noexcept = nullptr;
};

/// Loop-thread lifetime token. A posted infrastructure callback must check this
/// token on the loop, never retain a raw Nursery beyond its admission lifetime.
struct NurseryLifetime {
    Nursery* nursery = nullptr;
};

/// Infrastructure-only ingress token; read exclusively by loop callbacks.
struct NurseryAccess {
    static std::shared_ptr<NurseryLifetime> lifetime(Nursery& nursery) noexcept;
};

/// The nursery's bookkeeping, living in the runner's frame.
///
/// It owns the child entries for as long as their results may still be claimed;
/// execution itself lives in the loop's registry. `active_count` counts the
/// children that have not retired yet, which is what a drain waits for.
struct NurseryState {
    NurseryState(Loop* loop_ptr, CancelScope* parent, NurseryPolicy policy)
        : loop(loop_ptr), supervisor(policy == NurseryPolicy::SUPERVISOR), scope(parent, false) {}

    NurseryState(const NurseryState&) = delete;
    NurseryState& operator=(const NurseryState&) = delete;

    /// Requires a drained nursery: every child retired before the runner
    /// released its bookkeeping, so no entry can hold a live record here.
    ~NurseryState() noexcept {
        assert(all_done() && children.empty() && retired_entry == nullptr && "nursery destroyed before cleanup");
        if (!all_done() || !children.empty() || retired_entry != nullptr) {
            std::terminate();
        }
    }

    void close() noexcept {
        accepting = false;
        lifetime->nursery = nullptr;
    }

    /// Revoke every result before destroying any payload, which may reenter.
    void clear_results() noexcept {
        assert(all_done() && next_head == nullptr && done_waiter == nullptr);
        close();
        for (auto& entry : children) {
            entry->result()->expire();
            entry->owner = nullptr;
            entry->in_completed = false;
            entry->completed_next = nullptr;
            entry->completed_prev = nullptr;
        }
        completed_head = nullptr;
        completed_tail = nullptr;
        for (auto& entry : children) {
            entry->clear_payload(*entry);
        }
        children.clear();
        retired_entry.reset();
    }

    /// READY fast paths obey the same structural boundary as parked consumers.
    void validate_consumer(const ExecutionRecord& caller) const {
        if (caller.loop() != loop) {
            throw std::logic_error("nursery results require the owning loop");
        }
        for (const ExecutionRecord* ancestor = &caller; ancestor != nullptr; ancestor = ancestor->parent()) {
            if (ancestor == runner) {
                return;
            }
        }
        throw std::logic_error("nursery results require a structured descendant");
    }

    Loop* loop = nullptr;
    bool supervisor = false;
    bool accepting = true;
    CancelScope scope;
    /// Execution record of the runner frame; the parent of every child.
    ExecutionRecord* runner = nullptr;
    std::vector<std::shared_ptr<ChildEntryBase>> children;
    /// The entry released by the previous discard retirement. Its storage must
    /// outlive the retirement callback, so it is dropped at the next safe point.
    /// One slot is enough: the reaper retires one record at a time.
    std::shared_ptr<ChildEntryBase> retired_entry;
    std::shared_ptr<NurseryLifetime> lifetime = std::make_shared<NurseryLifetime>();
    ChildEntryBase* completed_head = nullptr;
    ChildEntryBase* completed_tail = nullptr;
    NextWaiter* next_head = nullptr;
    /// The single drain waiter: it resumes when the last child retires.
    WaitCore* done_waiter = nullptr;
    std::exception_ptr first_error;
    std::size_t active_count = 0;

    [[nodiscard]] bool all_done() const noexcept { return active_count == 0; }

    /// Reserve the entry allocation and its registry slot. Fallible: this is
    /// the preparation an activation performs before it commits.
    template<typename T>
    std::shared_ptr<ChildEntry<T>> prepare_child(const void* tag) {
        auto entry = std::make_shared<ChildEntry<T>>();
        entry->owner = this;
        entry->index = children.size();
        entry->type_tag = tag;
        // push_back's geometric growth is still preparation: failure leaves no
        // activated child, while reserving size()+1 here would reallocate per spawn.
        children.push_back(std::move(entry));
        ++active_count;
        return std::static_pointer_cast<ChildEntry<T>>(children.back());
    }

    /// Terminal role: the child reached final_suspend. Fail-fast bookkeeping
    /// happens here — at termination, not at retirement — so siblings are
    /// cancelled at the same moment as before.
    void child_terminal(ChildEntryBase& entry) noexcept;

    /// Retirement role: the reap transaction destroyed the child's frame and
    /// published its result. The completion queue, the drain count and the
    /// waiters move only now, because only now may anyone consume the result.
    void child_retired(ChildEntryBase& entry) noexcept;

    void wake_next(const void* tag) noexcept;

    void unlink_completed(ChildEntryBase& entry) noexcept {
        if (!entry.in_completed) {
            return;
        }
        if (entry.completed_prev != nullptr) {
            entry.completed_prev->completed_next = entry.completed_next;
        } else {
            completed_head = entry.completed_next;
        }
        if (entry.completed_next != nullptr) {
            entry.completed_next->completed_prev = entry.completed_prev;
        } else {
            completed_tail = entry.completed_prev;
        }
        entry.completed_next = nullptr;
        entry.completed_prev = nullptr;
        entry.in_completed = false;
    }

    /// Append an entry to the completion queue in retirement order.
    void link_completed(ChildEntryBase& entry) noexcept {
        entry.completed_prev = completed_tail;
        entry.in_completed = true;
        if (completed_tail != nullptr) {
            completed_tail->completed_next = &entry;
        } else {
            completed_head = &entry;
        }
        completed_tail = &entry;
    }

    /// Claim the oldest completed child of type `tag`, in retirement order, and
    /// reserve its result for this consumer without changing its queue position.
    /// A cancelled child has no value for next<T>(): it is unlinked but left
    /// available for a Handle.
    ChildEntryBase* claim_completed(const void* tag) noexcept {
        ChildEntryBase* entry = completed_head;
        while (entry != nullptr) {
            ChildEntryBase* const next = entry->completed_next;
            if (entry->type_tag == tag && entry->available()) {
                if (entry->result()->content() == ResultContent::CANCELLED) {
                    unlink_completed(*entry);
                } else if (entry->result()->reserve()) {
                    return entry;
                }
            }
            entry = next;
        }
        return nullptr;
    }

    /// True while some matching result may still become claimable by a parked
    /// waiter, so that waiter stays parked instead of being told it is exhausted.
    ///
    /// Two things qualify: a live producer (its retirement wakes this list), and a
    /// published result that another consumer currently *reserves* — a reservation
    /// is handed back on cancellation and consumed otherwise, and both paths wake
    /// this list again. Without the second case a waiter behind a claim would be
    /// told `nullopt` while the very value it waited for was still in the
    /// completion queue, and the release that followed could no longer reach it.
    ///
    /// A still-running child whose pending outcome a Handle already reserved stays
    /// out of scope: it publishes nothing a next<T>() consumer could take, so it
    /// must not keep a waiter parked on someone else's reservation. A retired
    /// result reserved by a Handle is tracked here, because that reservation can
    /// come back into the queue the same way.
    [[nodiscard]] bool has_pending(const void* tag) const noexcept {
        for (const auto& entry : children) {
            if (entry->owner == nullptr || entry->type_tag != tag) {
                continue;
            }
            if (entry->record.phase() == ExecutionPhase::RETIRED) {
                if (entry->result()->consumption() == ResultConsumption::RESERVED) {
                    return true;
                }
                continue;  // consumed, expired, or a cancelled result next<T>() skips
            }
            if (entry->available()) {
                return true;
            }
        }
        return false;
    }

    /// Return a reservation without changing the original publication order.
    /// Claimed entries stay linked until consumption, so no reinsertion is needed.
    /// Handing the reservation back makes the result claimable again, so the
    /// waiters that were told to keep waiting for it are woken here.
    void release_claim(ChildEntryBase& entry) noexcept {
        if (entry.owner == nullptr) {
            return;
        }
        if (entry.result()->consumption() == ResultConsumption::RESERVED) {
            entry.result()->release_reservation();
            wake_next(entry.type_tag);
        }
    }

    void add_next_waiter(NextWaiter& waiter) noexcept {
        waiter.next = next_head;
        if (next_head != nullptr) {
            next_head->prev = &waiter;
        }
        next_head = &waiter;
        waiter.linked = true;
    }

    void remove_next_waiter(NextWaiter& waiter) noexcept {
        if (!waiter.linked) {
            return;
        }
        if (waiter.prev != nullptr) {
            waiter.prev->next = waiter.next;
        } else {
            next_head = waiter.next;
        }
        if (waiter.next != nullptr) {
            waiter.next->prev = waiter.prev;
        }
        waiter.next = nullptr;
        waiter.prev = nullptr;
        waiter.linked = false;
    }

    /// Drop a consumed entry from the nursery. The frame is already gone — the
    /// reap transaction destroyed it — so this only releases bookkeeping and the
    /// last handle on the entry's storage.
    void retire(ChildEntryBase& entry) noexcept {
        assert(entry.owner == this && "retiring an entry that is not owned here");
        const void* const type_tag = entry.type_tag;
        // The registry owns this entry through payload cleanup; erase_entry()
        // takes its index before dropping that owner and never touches it after.
        unlink_completed(entry);
        entry.owner = nullptr;
        entry.clear_payload(entry);
        erase_entry(entry);
        // The result a parked next<T>() waiter was waiting for is gone for good:
        // a consumed reservation can have been the last one, so its exhaustion
        // answer has to be delivered here instead of being waited on.
        wake_next(type_tag);
    }

    /// A discarded child has no consumer. Keep its storage alive through the
    /// current retirement callback, then release it at the next safe point.
    void retire_discard(ChildEntryBase& entry) noexcept {
        std::shared_ptr<ChildEntryBase> keep = std::move(children[entry.index]);
        retire(entry);
        retired_entry = std::move(keep);  // shared_ptr move: no allocation
    }

    void erase_entry(ChildEntryBase& entry) noexcept {
        const std::size_t index = entry.index;
        if (index + 1 != children.size()) {
            children[index] = std::move(children.back());
            children[index]->index = index;
        }
        children.pop_back();
    }

    /// Only unobserved supervised failures remain here: an observed result
    /// retired its entry, and a failed discard child is deliberately retained
    /// for exit. Allocates, so it is called outside the reap transaction.
    [[nodiscard]] std::vector<std::exception_ptr> unobserved_errors() const {
        std::vector<std::exception_ptr> errors;
        for (const auto& entry : children) {
            if (entry->failed()) {
                errors.push_back(entry->result()->error());
            }
        }
        return errors;
    }
};

/// Exception-safe release of a claimed entry: bookkeeping is dropped even when
/// moving the payload out throws.
struct ConsumedEntry {
    ChildEntryBase& entry;

    ~ConsumedEntry() noexcept {
        if (entry.owner != nullptr) {
            entry.owner->retire(entry);
        }
    }
};

inline void NurseryState::wake_next(const void* tag) noexcept {
    if (tag == nullptr) {
        return;
    }
    NextWaiter* waiter = next_head;
    while (waiter != nullptr) {
        NextWaiter* const next = waiter->next;
        if (waiter->type_tag == tag && waiter->try_satisfy != nullptr) {
            if (!waiter->try_satisfy(*waiter)) {
                // No matching result is available, but a producer remains.
                // No result became available: later waiters of this type fail too.
                // Exhaustion returns true, so every exhausted waiter still wakes.
                return;
            }
        }
        waiter = next;
    }
}

inline void NurseryState::child_terminal(ChildEntryBase& entry) noexcept {
    // Only the first defect cancels siblings; a cancelled child is not a defect.
    if (!supervisor && !first_error && entry.failed()) {
        first_error = entry.result()->error();
        close();
        ScopeAccess::cancel(scope, CancelCause::REQUESTED);
    }
}

inline void NurseryState::child_retired(ChildEntryBase& entry) noexcept {
    // The reaper no longer refers to the entry released by the previous
    // retirement, so this is the first safe point to drop it.
    retired_entry.reset();
    --active_count;
    if (WaitOperation* waiter = std::exchange(entry.join_waiter, nullptr); waiter != nullptr) {
        waiter->event_ready();
    }
    const void* const type_tag = entry.type_tag;
    if (entry.discard && !entry.failed()) {
        // A discarded outcome nobody may claim: release its bookkeeping, but
        // keep the entry alive until the retirement callback has returned.
        retire_discard(entry);
    } else {
        link_completed(entry);
        // A cancelled result still counts as retirement: typed waiters must
        // re-check and either claim the next result or report exhaustion.
        wake_next(type_tag);
    }
    if (done_waiter != nullptr && all_done()) {
        std::exchange(done_waiter, nullptr)->event_ready();
    }
}

/// Terminal notification of a nursery child: fail-fast bookkeeping only. It runs
/// from final_suspend, while the frame still exists; `entry.owner` is valid
/// because an entry is only released after its result was consumed or the
/// nursery exited, both of which need the result first.
inline void nursery_child_terminal(void* context, ExecutionRecord&) noexcept {
    auto& entry = *static_cast<ChildEntryBase*>(context);
    entry.owner->child_terminal(entry);
}

/// Retirement notification of a nursery child: the result is published, so the
/// completion queue, the drain count and the waiters may move now.
///
/// A discarded entry is kept alive by NurseryState::retired_entry until the next
/// safe point; the reaper takes no lease, and this callback may drop its
/// ownership.
inline void nursery_child_retired(void* context, ExecutionRecord&) noexcept {
    auto& entry = *static_cast<ChildEntryBase*>(context);
    entry.owner->child_retired(entry);
}

/// Internal drain is intentionally not a cancellation checkpoint. Children must
/// finish before their owner or borrowed outer resources can be destroyed.
struct AllDoneAwaitable {
    NurseryState* state = nullptr;
    WaitCore wait{};

    bool await_ready() const noexcept { return state->all_done(); }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        assert(state->done_waiter == nullptr && "only one nursery drain may be active");
        if (state->done_waiter != nullptr) {
            std::terminate();
        }
        wait.begin(*handle.promise().exec);
        state->done_waiter = &wait;
        wait.commit_suspend();
    }

    void await_resume() noexcept {
        wait.begin_resume();
        wait.finish();
    }
};

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
#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_STATE_HPP
