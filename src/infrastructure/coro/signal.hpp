//
// Coroutine runtime — signals as coroutine events.
//
// on_signal parks the awaiting frame; the process-wide sigaction handler only
// latches a pending bit and writes one byte to the loop's self-pipe, and the
// loop turns that into an ordinary coroutine resumption. There is no signal
// thread and no second notification domain. Users loop for repeated handling.
//
// Process-wide state: the handler cannot capture, so it reaches its loop
// through two file-scope atomics declared in loop.cpp. See the justification
// there.
//
// The wait is cancellable (it registers on the awaiting scope), so a scope
// timeout ends a pending signal wait by throwing Cancelled from Task<void>.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_SIGNAL_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_SIGNAL_HPP

#include "infrastructure/coro/detail/signal_awaitable.h"
#include "infrastructure/coro/task.hpp"

namespace coro {

/// Complete once the process receives `sig`; cancelled if the scope is.
///
/// The signal handler is process-wide and installed for the lifetime of the
/// loop, so a caller must not also rely on the default disposition of `sig`.
/// Failure: allocation may throw; invalid/uncatchable signals throw
/// std::invalid_argument, and handler installation failure throws std::system_error.
/// A failed registration leaves no waiter or changed signal disposition.
[[nodiscard]] inline Task<void> on_signal(int sig) {
    co_return co_await detail::SignalAwaitable{sig};
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_SIGNAL_HPP