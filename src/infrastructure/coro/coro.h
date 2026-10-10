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
#include "infrastructure/coro/async_mutex.hpp"
#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/checkpoint.hpp"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/fd_wait.hpp"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/mutex_guard.hpp"
#include "infrastructure/coro/now.hpp"
#include "infrastructure/coro/offload.hpp"
#include "infrastructure/coro/run.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/scope_outcome.hpp"
#include "infrastructure/coro/signal.hpp"
#include "infrastructure/coro/sleep.hpp"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/coro/task_group.hpp"
#include "infrastructure/coro/time.h"
// IWYU pragma: end_exports

#endif  // YADDNSC_INFRASTRUCTURE_CORO_CORO_H
