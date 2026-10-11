// Coroutine runtime — the offload job cell and awaitable behind coro::offload.
//
// The job holds a shared_ptr to the callable and to the result cell, so both
// outlive an abandoned await. Atomic cancellation/completion and stop state
// cross threads; all waiter registration is confined to the loop.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H

#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <stop_token>
#include <type_traits>

#include <coroutine>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/result_box.hpp"
#include "coro/detail/wait_node.h"
#include "coro/loop.h"

namespace coro::detail {

/// State shared between the loop-side awaiter and the worker job.
///
/// Ownership: held by std::shared_ptr on both sides, so an abandoned await can
/// return (and its frame die) while the worker is still running; the job's copy
/// outlives it. `waiter`/`awaiting`/`node` are loop-thread state. Cancellation
/// flags and stop_source synchronize cancellation with the worker; completed's
/// release/acquire publishes its result/error.
struct OffloadSlot {
    std::exception_ptr error;
    std::atomic<bool> abandoned{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};
    std::stop_source stop;
    bool join_on_cancel = false;
    PromiseBase* waiter = nullptr;
    bool awaiting = false;
    WaitNode node{};
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
    if (state.awaiting && state.waiter != nullptr && !state.node.scheduled) {
        state.node.scheduled = true;
        wake(*state.waiter);
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
    using R = std::invoke_result_t<Fn>;

    std::shared_ptr<Fn> fn;
    std::shared_ptr<OffloadState<R>> state = std::make_shared<OffloadState<R>>();

    constexpr bool await_ready() const noexcept { return false; }

    /// Submits the job unless the scope is already cancelled, then parks.
    /// Allocates (shared state, queue node), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        Loop* loop = promise.loop;
        CancelScope* scope = promise.scope;
        state->node.scope = scope;
        state->join_on_cancel = Joined;
        state->node.wake_on_cancel = !Joined;
        assert(loop != nullptr && "offload must be awaited inside coro::run");
        if (scope != nullptr && scope->cancelled()) {
            return false;
        }
        state->waiter = &promise;
        state->awaiting = true;
        if (scope != nullptr) {
            state->node.waiter = &promise;
            state->node.owner = state.get();
            state->node.on_cancel = &OffloadAwaitable::on_cancel;
            ScopeAccess::add_waiter(*scope, state->node);
        }
        std::shared_ptr<Fn> work = fn;
        std::shared_ptr<OffloadState<R>> shared = state;
        try {
            LoopAccess::submit_offload(*loop, [work, shared, loop]() mutable {
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
                try {
                    loop->post([shared] { offload_finish(*shared); });
                } catch (...) {
                    // Losing a completion would leave its await parked forever.
                    std::terminate();
                }
            });
        } catch (...) {
            if (scope != nullptr) {
                ScopeAccess::remove_waiter(*scope, state->node);
            }
            throw;
        }
        return true;
    }

    /// Yields the job's value, or `coro::Cancelled` when abandoned, or
    /// rethrows the defect the callable threw (possibly T's throwing move).
    R await_resume() {
        if (state->node.linked && state->node.scope != nullptr) {
            ScopeAccess::remove_waiter(*state->node.scope, state->node);
        }
        if (state->node.scope != nullptr && state->node.scope->cancelled()) {
            // A completed job can be abandoned before its parent resumes too.
            state->abandoned.store(true, std::memory_order_release);
            state->node.scope->throw_if_cancelled();
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

    /// Cancellation hook: marks the job abandoned so a not-yet-started job is
    /// dropped by the worker. Never throws; runs on the loop thread.
    static void on_cancel(WaitNode& node) noexcept {
        auto* state = static_cast<OffloadState<R>*>(node.owner);
        state->cancelled.store(true, std::memory_order_release);
        if (state->join_on_cancel) {
            // request_stop may invoke callbacks inline: the public contract
            // requires them to be non-blocking and not re-enter the loop.
            state->stop.request_stop();
        } else {
            state->abandoned.store(true, std::memory_order_release);
        }
    }
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H
