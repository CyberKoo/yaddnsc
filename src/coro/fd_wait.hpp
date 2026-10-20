//
// Coroutine runtime — file-descriptor readiness waits.
//
// The runtime's fd table is callback-based; detail::FdAwaitable is the awaitable
// that turns one registration into a checkpoint: park the awaiting frame, resume
// it through the ready queue when poll() reports the requested direction, and
// surface a scope cancellation as `coro::Cancelled`. A wait can also fail
// without the fd ever becoming ready — when the loop's event source itself is
// impaired (a poll() ENOMEM that persists), the wait resolves to the poll errno
// instead of readiness, so callers fold it into their own operational error
// channel. A descriptor closed while the wait was parked resolves to
// std::errc::bad_file_descriptor through that same channel.
//
// This is the smallest increment the transport layer needs from the runtime: no
// timeout parameter (a deadline is the caller's cancel scope), no reactor object
// (`Loop` comes from the ambient context), and no second notification path (the
// registration is dropped before the frame is resumed).
//
// The returned operation may be reused; each await owns an independent
// registration.
//
// Awaiting outside coro::run is a programming defect: assertion-diagnosed in
// Debug builds, undefined behavior in Release. A negative `fd` is an argument
// error instead: the await throws std::invalid_argument.
//

#ifndef YADDNSC_CORO_FD_WAIT_HPP
#define YADDNSC_CORO_FD_WAIT_HPP

#include "coro/detail/fd_awaitable.h"  // IWYU pragma: export

namespace coro {

/// Suspend until `fd` is readable (or reports EOF/error as readable).
/// Resolves to an empty std::error_code on readiness, to the loop's poll
/// errno when the event source itself failed the wait, or to
/// std::errc::bad_file_descriptor when the descriptor was closed while the
/// wait was parked; callers must branch on the result. Throws
/// std::invalid_argument when `fd` is not a valid descriptor number.
/// Precondition: awaited inside coro::run (see the header note).
[[nodiscard]] inline auto wait_readable(int fd) noexcept {
    return detail::FdAwaitable::readable(fd);
}

/// Suspend until `fd` is writable. Same resolution contract as wait_readable.
/// Throws std::invalid_argument when `fd` is not a valid descriptor number.
/// Precondition: awaited inside coro::run (see the header note).
[[nodiscard]] inline auto wait_writable(int fd) noexcept {
    return detail::FdAwaitable::writable(fd);
}

}  // namespace coro

#endif  // YADDNSC_CORO_FD_WAIT_HPP