// Coroutine runtime — the offload job cell and awaitable behind coro::offload.
//
// The job holds a shared_ptr to the callable and to the result cell, so both
// outlive an abandoned await. Atomic cancellation/completion and stop state
// cross threads; all waiter registration is confined to the loop.
#ifndef YADDNSC_CORO_DETAIL_OFFLOAD_JOB_H
#define YADDNSC_CORO_DETAIL_OFFLOAD_JOB_H

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/inbox_node.h"
#include "coro/detail/offload_admission.h"
#include "coro/detail/promise_base.h"
#include "coro/detail/result_box.hpp"
#include "coro/detail/wait_operation.h"
#include "coro/loop.h"

namespace coro::detail {

/// State shared between the loop-side awaiter and the worker job.
///
/// Ownership: held by std::shared_ptr on both sides, so an abandoned await can
/// return (and its frame die) while the worker is still running; the job's copy
/// outlives it. `attached_waiter` is loop-thread state. Cancellation
/// flags and stop_source synchronize cancellation with the worker; completed's
/// release/acquire publishes its result/error.
struct OffloadSlot {
    std::exception_ptr error;
    std::atomic<bool> abandoned{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};
    std::stop_source stop;
    bool join_on_cancel = false;
    /// Loop-thread-only attachment to the awaiting frame's wait operation. It is
    /// cleared as soon as the wait is resolved, so a completion that arrives
    /// after an abandoned await has nothing to touch. The worker never reads it.
    WaitOperation* attached_waiter = nullptr;
    Loop* loop = nullptr;
    OffloadAdmission admission{};
    /// Embedded inbox node for the completion path: the worker posts it with
    /// zero allocation, so a completed job's wakeup can never be lost.
    InboxNode completion_node{};
    /// Self-lease keeping this state (and the embedded node) alive between the
    /// worker's post and the trampoline's run — an abandoned await drops its
    /// own reference earlier. The trampoline moves the lease out, breaking the
    /// cycle; loop teardown clears it through the node's dispose hook.
    std::shared_ptr<OffloadSlot> completion_lease;
};

template<typename R>
struct OffloadState final : OffloadSlot {
    ResultBox<R> result;
};

/// Loop-side completion: resume the awaiter unless the wait was abandoned.
///
/// Runs on the loop thread from the inbox. Never throws: it only reads atomics
/// and schedules. The acquire on `completed` pairs with the worker's release,
/// so the result cell written by the worker is visible here.
template<typename R>
void offload_finish(OffloadState<R>& state) noexcept {
    if (state.abandoned.load(std::memory_order_acquire)) {
        return;
    }
    if (!state.completed.load(std::memory_order_acquire)) {
        return;
    }
    // Detach before reporting: the wait operation lives in the awaiting frame,
    // and a second completion must not reach it.
    if (WaitOperation* waiter = state.attached_waiter; waiter != nullptr) {
        state.attached_waiter = nullptr;
        waiter->event_ready();
    }
}

/// Awaits one offload job.
///
/// Precondition: awaited inside coro::run (the frame must run on a loop).
/// Ownership: the job holds a shared_ptr to the callable and to the result
/// cell, so both outlive an abandoned await. The awaiter registers a scope
/// waiter. Abandon resumes on cancellation; Joined requests stop and stays
/// parked until completion, preserving the awaiting operation's resources.
/// Thread safety: await_suspend/await_resume run on the loop thread; the job
/// body runs on a pool worker and publishes the result before completed.
template<typename F, bool Joined = false>
struct OffloadAwaitable {
    using Fn = std::decay_t<F>;
    using R = std::invoke_result_t<Fn&>;

    std::shared_ptr<Fn> fn;
    std::shared_ptr<OffloadState<R>> state = std::make_shared<OffloadState<R>>();
    /// The wait this awaitable parks on. It lives in the awaiting frame, and the
    /// slot only borrows it through attached_waiter while the await is live.
    CancelSubscription subscription_{};
    WaitOperation wait_{subscription_};
    /// The scope this await parked on, kept for the resume-side priority check.
    CancelScope* scope_ = nullptr;

    constexpr bool await_ready() const noexcept { return false; }

    /// Submits the job unless the scope is already cancelled, then parks.
    /// Allocates (shared state, queue node), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        assert(promise.exec != nullptr && "offload must be awaited inside coro::run");
        Loop* loop = promise.exec->loop();
        CancelScope* scope = promise.exec->scope();
        state->loop = loop;
        scope_ = scope;
        state->join_on_cancel = Joined;
        assert(loop != nullptr && "offload must be awaited inside coro::run");
        if (scope != nullptr && scope->cancelled()) {
            return false;
        }
        wait_.begin(*promise.exec);
        if (scope != nullptr) {
            wait_.subscribe_cancel(*scope, state.get(), &OffloadAwaitable::on_cancel);
        }
        state->attached_waiter = &wait_;
        state->completion_node.context = state.get();
        state->completion_node.run = &OffloadAwaitable::completion_trampoline;
        state->completion_node.dispose = &OffloadAwaitable::dispose_completion;
        std::shared_ptr<Fn> work = std::move(fn);
        std::shared_ptr<OffloadState<R>> shared = state;
        try {
            state->admission.job = [work = std::move(work), shared, loop]() mutable {
                if (!shared->cancelled.load(std::memory_order_acquire)) {
                    try {
                        shared->result.invoke(*work);
                    } catch (...) {
                        shared->error = std::current_exception();
                    }
                }
                // Release the worker's callable lease before publishing done:
                // joined completion must not race callable capture destruction.
                work.reset();
                shared->completed.store(true, std::memory_order_release);
                // The self-lease keeps the state (and the embedded node) alive
                // until the loop runs the trampoline, which moves the lease out.
                // Posting is allocation-free and non-throwing: a completed job's
                // wakeup can never be lost.
                shared->completion_lease = shared;
                LoopAccess::post_node(*loop, shared->completion_node);
            };
            state->admission.owner = state.get();
            state->admission.failed = [](void* owner, std::exception_ptr error) noexcept {
                auto& failed = *static_cast<OffloadState<R>*>(owner);
                failed.error = std::move(error);
                failed.completed.store(true, std::memory_order_release);
                offload_finish(failed);
            };
            LoopAccess::admit_offload(*loop, state->admission);
        } catch (...) {
            state->admission.job = {};
            state->attached_waiter = nullptr;
            wait_.rollback();
            throw;
        }
        wait_.commit_suspend();
        return true;
    }

    /// Yields the job's value, or `coro::Cancelled` when abandoned, or
    /// rethrows the defect the callable threw (possibly T's throwing move).
    R await_resume() {
        wait_.begin_resume();
        wait_.finish();
        // The wait is resolved: the slot must not reach this frame again, which
        // is what lets an abandoned await return while the worker still runs.
        state->attached_waiter = nullptr;
        if constexpr (Joined) {
            // Joining owns the defect boundary even if its caller stopped waiting.
            if (state->error) {
                std::rethrow_exception(state->error);
            }
        }
        if (scope_ != nullptr && scope_->cancelled()) {
            // A completed job can be abandoned before its parent resumes too.
            state->abandoned.store(true, std::memory_order_release);
            scope_->throw_if_cancelled();
        }
        if (state->error) {
            std::rethrow_exception(state->error);
        }
        if constexpr (std::is_void_v<R>) {
            state->result.take();
            return;
        } else {
            return state->result.take();
        }
    }

    /// Inbox completion entry (loop thread): wake the awaiter unless the wait
    /// was abandoned. Worker-slot accounting is the loop's own concern and
    /// rides the submission wrapper, not this node.
    static void completion_trampoline(InboxNode& node) noexcept {
        auto& state = *static_cast<OffloadState<R>*>(node.context);
        // The worker posts only after taking the lease, and the loop runs or
        // disposes a node exactly once, so the lease is always still here.
        assert(state.completion_lease != nullptr && "offload completion ran without its self-lease");
        auto lease [[maybe_unused]] = std::move(state.completion_lease);  // breaks the self-reference
        offload_finish(state);
    }

    /// Teardown hook for an orphaned completion node: drop the self-lease
    /// without running the completion (the loop is gone).
    static void dispose_completion(InboxNode& node) noexcept {
        auto& state = *static_cast<OffloadState<R>*>(node.context);
        assert(state.completion_lease != nullptr && "offload node disposed without its self-lease");
        state.completion_lease.reset();
    }

    /// Cancellation hook: mark the job cancelled so a not-yet-started job is
    /// dropped by the worker, and decide whether the waiter may resume now.
    /// A joined wait stays parked until the job really finished, because the
    /// worker is still using the resources the awaiter owns; an abandon detaches
    /// the waiter first and resumes. Never throws; runs on the loop thread.
    static CancelAction on_cancel(void* context) noexcept {
        auto* state = static_cast<OffloadState<R>*>(context);
        state->cancelled.store(true, std::memory_order_release);
        if (!state->admission.submitted) {
            LoopAccess::cancel_offload(*state->loop, state->admission);
            state->join_on_cancel = false;  // no submitted work remains to join
        }
        if (state->join_on_cancel) {
            // request_stop may invoke callbacks inline: the public contract
            // requires them to be non-blocking and not re-enter the loop.
            state->stop.request_stop();
            return CancelAction::WAIT_EXTERNAL;  // the completion will resume
        }
        state->abandoned.store(true, std::memory_order_release);
        state->attached_waiter = nullptr;  // the wait ends here; the worker runs on
        return CancelAction::RESUME;
    }
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_OFFLOAD_JOB_H
