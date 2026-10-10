//
// Coroutine runtime — file-descriptor readiness waits.
//
// The runtime's fd table is callback-based; detail::FdAwaitable is the awaitable
// that turns one registration into a checkpoint: park the awaiting frame, resume
// it through the ready queue when poll() reports the requested direction, and
// surface a scope cancellation as `coro::Cancelled`.
//
// This is the smallest increment the transport layer needs from the runtime: no
// timeout parameter (a deadline is the caller's cancel scope), no reactor object
// (`Loop` comes from the ambient context), and no second notification path (the
// registration is dropped before the frame is resumed).
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_FD_WAIT_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_FD_WAIT_HPP

#include "coro/detail/fd_awaitable.h"  // IWYU pragma: export

namespace coro {

/// Suspend until `fd` is readable (or reports EOF/error as readable).
[[nodiscard]] inline auto wait_readable(int fd) noexcept {
    return detail::FdAwaitable::readable(fd);
}

/// Suspend until `fd` is writable.
[[nodiscard]] inline auto wait_writable(int fd) noexcept {
    return detail::FdAwaitable::writable(fd);
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_FD_WAIT_HPP