//
// Coroutine runtime — structured concurrency.
//
// Two combinators, one body. A task_group cancels the siblings when a child
// fails and rethrows the first defect once every child is joined; a
// supervisor_group leaves siblings running. Observed failures are delivered
// through next()/Handle; unobserved failures become GroupError at scope exit.
//
// `spawn` returns a Handle; `co_await handle` joins that child. `next()`
// consumes child results in completion order. `spawn_discard` starts a
// fire-and-forget child whose slot leaves the bookkeeping the moment it
// completes, so a group that lives for the whole process stays bounded by its
// live children instead of its history. Scope exit always joins every child
// and reaps its frame — there are no detached tasks.
//
// Keeping the body frame does NOT extend the lifetime of locals destroyed at
// body return/unwind. Put borrowed resources outside the group call, or use
// group.with_children() while resources in the outer body are still alive.
//
// This header holds only the two public combinators. The group types live in
// task_group.hpp (they must, because the body constructs a TaskGroup) and the
// body itself lives in detail/group_runner.hpp.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_GROUP_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_GROUP_HPP

#include <utility>

#include "coro/detail/group_runner.hpp"
#include "coro/group_error.h"  // IWYU pragma: export
#include "coro/task.hpp"
#include "coro/task_group.hpp"

namespace coro {

/// Structured group: a child failure cancels its siblings, then propagates.
///
/// `fn` receives the group by reference and returns the group body. The body
/// runs in the group's scope; scope exit joins every child and reaps every
/// frame. The first child defect is rethrown after all children are joined.
/// Thread safety: loop thread only. Failure: allocation may throw; the group
/// task may complete with a child's or the body's defect.
template<typename Fn>
[[nodiscard]] Task<void> task_group(Fn fn) {
    return detail::run_group<Fn, false>(std::move(fn));
}

/// Supervised group: a child failure is reported through next(), siblings run on.
///
/// Same structure and ownership as task_group; only the child-failure policy
/// differs: child defects never cancel siblings. Handle/next consumption observes
/// a failure; every remaining failure (including discard children) is reported
/// in GroupError after draining. Failed unobserved frames are kept until exit.
template<typename Fn>
[[nodiscard]] Task<void> supervisor_group(Fn fn) {
    return detail::run_group<Fn, true>(std::move(fn));
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_GROUP_HPP