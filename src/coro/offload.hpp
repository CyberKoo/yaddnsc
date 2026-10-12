//
// Coroutine runtime — offload.
//
// Work that may block or burn CPU leaves the loop through `offload`; there is no
// difference between blocking and CPU-bound work, so one primitive covers both.
// offload() is the only gateway business code has to the thread pool: the pool
// itself is BS::thread_pool (rule 02, Reuse Protocol) and is owned by the Loop,
// and jobs are submitted fire-and-forget through Loop::submit_offload(). There is
// no std::future anywhere — the shared result cell carries the outcome back
// through the loop's single cross-thread channel.
//
// The pool never refuses work (BS's queue is unbounded and submission cannot
// fail for capacity reasons). Cancellation is abandon: a queued-not-started job
// is dropped, a running job finishes with its result discarded, and the await
// surfaces `coro::Cancelled`. offload_joined instead waits for actual worker
// completion before cancellation unwinds, optionally supplying a stop_token.
// run() drains submitted jobs even after root completion, pumping the inbox.
// A defect thrown by the callable is rethrown at
// the await point — offload is not an exception channel of its own.
//
// Awaiting outside coro::run is a programming defect: assertion-diagnosed in
// Debug builds, undefined behavior in Release.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP

#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "coro/detail/offload_job.h"
#include "coro/task.hpp"

namespace coro {

namespace detail {

/// The lazy frame owns its callable, never a reference to a caller temporary.
template<typename Fn>
Task<std::invoke_result_t<Fn&>> run_offload(std::shared_ptr<Fn> fn) {
    co_return co_await OffloadAwaitable<Fn>{std::move(fn)};
}

template<typename Fn>
Task<std::invoke_result_t<Fn&>> run_joined_offload(std::shared_ptr<Fn> fn) {
    co_return co_await OffloadAwaitable<Fn, true>{std::move(fn)};
}

template<typename Fn>
Task<std::invoke_result_t<Fn&, std::stop_token>> run_cooperative_offload(std::shared_ptr<Fn> fn) {
    using R = std::invoke_result_t<Fn&, std::stop_token>;
    auto state = std::make_shared<OffloadState<R>>();
    auto work = [fn = std::move(fn), token = state->stop.get_token()]() mutable -> R {
        return std::invoke(*fn, token);
    };
    using Work = decltype(work);
    co_return co_await OffloadAwaitable<Work, true>{std::make_shared<Work>(std::move(work)), std::move(state)};
}

}  // namespace detail

/// Run `fn` on the offload pool; the result returns through the loop.
/// Acquires ownership of the callable immediately, before the lazy task runs.
/// Invokes the owned callable as an lvalue on the worker.
/// Captured references must outlive the worker, including after cancellation.
///
/// Cancellation: abandon (see above) — the await throws
/// `coro::Cancelled`. Failure: allocation may throw; a defect
/// thrown by `fn` is rethrown at the await point.
/// Precondition: awaited inside coro::run (see the header note).
template<typename F>
[[nodiscard]] auto offload(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>&>> {
    return detail::run_offload(std::make_shared<std::decay_t<F>>(std::forward<F>(fn)));
}

/// Offload with structured worker lifetime: cancellation requests stop, but the
/// await does not resume until queued/running work has finished. Then sticky
/// cancellation is rethrown. Borrowed resources need only outlive this await.
/// A no-argument callable cannot be interrupted once running. A callable taking
/// std::stop_token may cooperate; token callbacks must not block or re-enter the
/// loop because cancellation requests originate on the loop thread. Neither
/// form guarantees a wall-clock limit for uncooperative synchronous code.
/// Owns the callable immediately; allocation/worker defects may throw. Loop
/// thread only at await; the callable executes on a pool worker.
template<typename F>
    requires std::is_invocable_v<std::decay_t<F>&>
[[nodiscard]] auto offload_joined(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>&>> {
    return detail::run_joined_offload(std::make_shared<std::decay_t<F>>(std::forward<F>(fn)));
}

/// Cooperative form of offload_joined (see its lifetime/callback contract).
template<typename F>
    requires(!std::is_invocable_v<std::decay_t<F>&> && std::is_invocable_v<std::decay_t<F>&, std::stop_token>)
[[nodiscard]] auto offload_joined(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>&, std::stop_token>> {
    return detail::run_cooperative_offload(std::make_shared<std::decay_t<F>>(std::forward<F>(fn)));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP