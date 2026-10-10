//
// Coroutine runtime — umbrella header.
//
// Everything a composition-layer caller needs: the loop entry point, Task and
// its structured scopes, cancellation combinators, sleeps, AsyncMutex, offload
// and signals.
//
// Thread safety: everything here is loop-thread only except Loop::post and the
// offload worker boundary, which are documented at their declarations.
//
// Dependency rule for this module
// ------------------------------
// `detail/` is a namespace and directory inside this module, not a separate
// layer. It holds shared runtime implementation types; private helper types and
// state may remain in the public class that owns them.
//
// Public headers include detail headers where template instantiation or
// co_await requires complete definitions. Detail headers may include the public
// types they implement, but the include graph must remain acyclic. For example,
// the mutex awaiter depends on MutexGuard and MutexState, not async_mutex.hpp.
// TaskPromise::get_return_object() is defined after Task in task.hpp because it
// constructs that public type.
//
// Application code uses only the admitted public APIs, not coro::detail or
// loop/clock implementation objects. Composition and infrastructure retain the
// runtime access needed for startup and I/O. The coroutine boundary checks in
// cmake/ArchitectureGuard.cmake cover application code; the architectural
// boundary is documented in docs/architecture.md.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_CORO_H
#define YADDNSC_INFRASTRUCTURE_CORO_CORO_H

// IWYU pragma: begin_exports
#include "coro/async_mutex.hpp"
#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/checkpoint.hpp"
#include "coro/clock.h"
#include "coro/fd_wait.hpp"
#include "coro/fwd.h"
#include "coro/group.hpp"
#include "coro/loop.h"
#include "coro/mutex_guard.hpp"
#include "coro/now.hpp"
#include "coro/offload.hpp"
#include "coro/run.hpp"
#include "coro/scope.hpp"
#include "coro/scope_outcome.hpp"
#include "coro/signal.hpp"
#include "coro/sleep.hpp"
#include "coro/task.hpp"
#include "coro/task_group.hpp"
#include "coro/time.h"
// IWYU pragma: end_exports

#endif  // YADDNSC_INFRASTRUCTURE_CORO_CORO_H
