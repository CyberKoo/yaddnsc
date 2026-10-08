//
// Coroutine runtime — offload.
//
// Work that may block or burn CPU leaves the loop through `offload`; there is no
// difference between blocking and CPU-bound work, so one primitive covers both.
// offload() is the only gateway business code has to the thread pool: the pool
// itself is BS::thread_pool (rule 02, Reuse Protocol) and is owned by the Loop,
// and jobs are submitted fire-and-forget via detach_task. There is no
// std::future anywhere — the shared result cell carries the outcome back
// through the loop's single cross-thread channel.
//
// The pool never refuses work (BS's queue is unbounded and submission cannot
// fail for capacity reasons). Cancellation is abandon: a queued-not-started job
// is dropped, a running job finishes with its result discarded, and the await
// surfaces `operation_canceled`. A defect thrown by the callable is rethrown at
// the await point — offload is not an exception channel of its own.
//

#ifndef YADDNSC_CORO_OFFLOAD_HPP
#define YADDNSC_CORO_OFFLOAD_HPP

#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/result_box.hpp"
#include "infrastructure/coro/task.hpp"

namespace coro {

/// offload's failure vocabulary: cancellation. Defects travel as exceptions.
using OffloadError = std::errc;

namespace detail {

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

}  // namespace detail

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
    std::shared_ptr<detail::OffloadState<R>> state = std::make_shared<detail::OffloadState<R>>();

    bool await_ready() const noexcept { return false; }

    /// Submits the job unless the scope is already cancelled, then parks.
    /// Allocates (shared state, queue node), so it may throw.
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        PromiseBase& promise = handle.promise();
        Loop* loop = promise.loop;
        CancelScope* scope = promise.scope;
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
            scope->add_waiter(state->node);
        }
        std::shared_ptr<Fn> work = fn;
        std::shared_ptr<detail::OffloadState<R>> shared = state;
        loop->offload_pool().detach_task([work, shared, loop] {
            if (!shared->abandoned.load(std::memory_order_acquire)) {
                try {
                    shared->result.invoke(*work);
                } catch (...) {
                    shared->error = std::current_exception();
                }
            }
            shared->completed.store(true, std::memory_order_release);
            loop->post([shared] { detail::offload_finish(*shared); });
        });
        return true;
    }

    /// Yields the job's value, or `operation_canceled` when abandoned, or
    /// rethrows the defect the callable threw (possibly T's throwing move).
    std::expected<R, std::errc> await_resume() {
        if (state->node.linked && state->node.scope != nullptr) {
            state->node.scope->remove_waiter(state->node);
        }
        if (state->cancelled) {
            return std::unexpected(std::errc::operation_canceled);
        }
        if (state->error) {
            std::rethrow_exception(state->error);
        }
        if constexpr (std::is_void_v<R>) {
            state->result.take();
            return {};
        } else {
            return state->result.take();
        }
    }

    /// Cancellation hook: marks the job abandoned so a not-yet-started job is
    /// dropped by the worker. Never throws; runs on the loop thread.
    static void on_cancel(WaitNode& node) noexcept {
        auto* state = static_cast<detail::OffloadState<R>*>(node.owner);
        state->abandoned.store(true, std::memory_order_release);
    }
};

/// Run `fn` on the offload pool; the result returns through the loop.
///
/// Cancellation: abandon (see above) — the await yields
/// `unexpected(operation_canceled)`. Failure: allocation may throw; a defect
/// thrown by `fn` is rethrown at the await point.
template<typename F>
[[nodiscard]] auto offload(F&& fn) -> Task<std::expected<std::invoke_result_t<std::decay_t<F>>, std::errc>> {
    co_return co_await OffloadAwaitable<std::decay_t<F>>{std::make_shared<std::decay_t<F>>(std::forward<F>(fn))};
}

}  // namespace coro

#endif  // YADDNSC_CORO_OFFLOAD_HPP
