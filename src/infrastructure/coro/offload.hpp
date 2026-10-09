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
// surfaces `coro::Cancelled`. A defect thrown by the callable is rethrown at
// the await point — offload is not an exception channel of its own.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP

#include <memory>
#include <type_traits>
#include <utility>

#include "infrastructure/coro/detail/offload_job.h"
#include "infrastructure/coro/task.hpp"

namespace coro {

namespace detail {

/// The lazy frame owns its callable, never a reference to a caller temporary.
template<typename Fn>
Task<std::invoke_result_t<Fn>> run_offload(std::shared_ptr<Fn> fn) {
    co_return co_await OffloadAwaitable<Fn>{std::move(fn)};
}

}  // namespace detail

/// Run `fn` on the offload pool; the result returns through the loop.
/// Acquires ownership of the callable immediately, before the lazy task runs.
/// Captured references must outlive the worker, including after cancellation.
///
/// Cancellation: abandon (see above) — the await throws
/// `coro::Cancelled`. Failure: allocation may throw; a defect
/// thrown by `fn` is rethrown at the await point.
template<typename F>
[[nodiscard]] auto offload(F&& fn) -> Task<std::invoke_result_t<std::decay_t<F>>> {
    return detail::run_offload(std::make_shared<std::decay_t<F>>(std::forward<F>(fn)));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_OFFLOAD_HPP