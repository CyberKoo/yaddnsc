//
// Coroutine runtime — umbrella header.
//
// Everything a composition-layer caller needs: the loop entry point, Task and
// its structured scopes, cancellation combinators, sleeps, AsyncMutex, offload,
// SerialLane and signals. No I/O yet — transport primitives arrive in stage 2.
//
// Thread safety: everything here is loop-thread only except Loop::post and the
// offload worker boundary, which are documented at their declarations.
//

#ifndef YADDNSC_CORO_CORO_H
#define YADDNSC_CORO_CORO_H

#include "infrastructure/coro/async_mutex.hpp"
#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/fd_wait.hpp"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/offload.hpp"
#include "infrastructure/coro/result_box.hpp"
#include "infrastructure/coro/run.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/serial_lane.hpp"
#include "infrastructure/coro/signal.hpp"
#include "infrastructure/coro/sleep.hpp"
#include "infrastructure/coro/task.hpp"

#endif  // YADDNSC_CORO_CORO_H
