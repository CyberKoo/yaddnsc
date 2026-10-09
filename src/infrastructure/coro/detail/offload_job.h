// Coroutine runtime — the offload job cell and awaitable behind coro::offload.
//
// The job holds a shared_ptr to the callable and to the result cell, so both
// outlive an abandoned await; the two atomics are the only cross-thread state.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H

#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include <coroutine>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/frame.h"
#include "infrastructure/coro/detail/result_box.hpp"
#include "infrastructure/coro/detail/wait_node.h"
#include "infrastructure/coro/loop.h"

namespace coro::detail {

/// State shared between the loop-side awaiter and the worker job.
///
/// Ownership: held by std::shared_ptr on both sides, so an abandoned await can
/// return (and its frame die) while the worker is still running; the job's copy
/// outlives it. `waiter`/`awaiting`/`node` are loop-thread state; `abandoned`
/// and `completed` are the only cross-thread fields, both atomics.
struct OffloadSlot {
    std::exception_ptr error;
    std::atomic<bool> abandoned{false};
    std::atomic<bool> completed{false};
    PromiseBase* waiter = nullptr;
    bool awaiting = false;
    bool cancelled = false;
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
/// Ownership: the job holds a shared_ptr to the callable and to the result
/// cell, so both outlive an abandoned await. The awaiter registers a scope
/// waiter; on cancellation the wait stops but the job is not interrupted.
/// Thread safety: await_suspend/await_resume run on the loop thread; the job
/// body runs on a pool worker and only touches the shared cell's atomics.
template<typename F>
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
        assert(loop != nullptr && "offload must be awaited inside coro::run");
        if (scope != nullptr && scope->cancelled()) {
            state->cancelled = true;
            return false;
        }
        state->waiter = &promise;
        state->awaiting = true;
        if (scope != nullptr) {
            state->node.waiter = &promise;
            state->node.cancelled_flag = &state->cancelled;
            state->node.owner = state.get();
            state->node.on_cancel = &OffloadAwaitable::on_cancel;
            ScopeAccess::add_waiter(*scope, state->node);
        }
        std::shared_ptr<Fn> work = fn;
        std::shared_ptr<OffloadState<R>> shared = state;
        try {
            LoopAccess::submit_offload(*loop, [work, shared, loop] {
                if (!shared->abandoned.load(std::memory_order_acquire)) {
                    try {
                        shared->result.invoke(*work);
                    } catch (...) {
                        shared->error = std::current_exception();
                    }
                }
                shared->completed.store(true, std::memory_order_release);
                loop->post([shared] { offload_finish(*shared); });
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
        state->abandoned.store(true, std::memory_order_release);
    }
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_OFFLOAD_JOB_H