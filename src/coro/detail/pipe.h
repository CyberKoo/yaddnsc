// Coroutine runtime — the wake-pipe factory behind the loop's self-pipe and the
// process stop signals.
//
// The runtime owns every pipe it uses: none is ever handed to application code.
// The pipe2() availability probe stays inside the implementation file, so a
// generic fd header that only offers ownership never leaks a platform macro to
// its includers.
#ifndef YADDNSC_CORO_DETAIL_PIPE_H
#define YADDNSC_CORO_DETAIL_PIPE_H

#include <utility>

#include "support/util/fd.hpp"

namespace coro::detail {

/// Create a non-blocking pipe (see pipe(2)) and return both ends as
/// RAII wrappers.
///
/// Both ends have O_NONBLOCK and close-on-exec set. Non-blocking keeps
/// cancellation signalling safe in noexcept paths: the single latch byte
/// never blocks a trigger, and cancellation readers only poll it (they
/// never consume the byte).
///
/// Returns a pair of (read_end, write_end).
/// On failure, both fds are invalid (operator bool returns false for both).
[[nodiscard]] std::pair<Utils::UniqueFd, Utils::UniqueFd> make_pipe() noexcept;

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_PIPE_H
