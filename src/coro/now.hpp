//
// Coroutine runtime — reading the loop clock.
//
// current_time() is how a coroutine body reads time: it yields the loop
// clock's now without handing out the Loop (trio exposes trio.current_time()
// for the same reason — time is a runtime service, not a runtime object).
//
// Awaiting outside coro::run is a programming defect: assertion-diagnosed in
// Debug builds, undefined behavior in Release.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_NOW_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_NOW_HPP

#include "coro/detail/context_awaitables.h"  // IWYU pragma: export

namespace coro {

/// The loop clock's current time.
/// Precondition: awaited inside coro::run (see the header note).
[[nodiscard]] inline auto current_time() noexcept {
    return detail::CurrentTime{};
}

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_NOW_HPP
