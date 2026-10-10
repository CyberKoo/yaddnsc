//
// Coroutine runtime — the explicit cancellation checkpoint.
//
// checkpoint() is the only cancellation point for code that computes rather than
// waits: it yields to the loop (so the stack cannot grow) and re-observes the
// sticky cancellation flag of the enclosing scope.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_CHECKPOINT_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_CHECKPOINT_HPP

#include "coro/detail/context_awaitables.h"  // IWYU pragma: export

namespace coro {

/// Yield to the loop and observe sticky cancellation. Loop thread only.
/// Use in long computations without another cancellable wait; throws Cancelled.
[[nodiscard]] inline auto checkpoint() noexcept {
    return detail::CheckpointAwaitable{};
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_CHECKPOINT_HPP