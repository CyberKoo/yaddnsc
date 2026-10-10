//
// Coroutine runtime — the RAII guard an AsyncMutex lock hands back.
//
// A top-level type rather than a nested `AsyncMutex::Guard`, because the lock
// awaitable has to be defined without AsyncMutex being complete (see
// detail/mutex_state.h) and may not name anything nested inside it. The nested
// alias remains for callers that prefer to spell it through the mutex.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_MUTEX_GUARD_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_MUTEX_GUARD_HPP

#include <utility>

#include "coro/detail/mutex_state.h"

namespace coro {

namespace detail {
class AsyncMutexLockAwaitable;
}  // namespace detail

/// RAII ownership of an AsyncMutex; unlocks on destruction or `unlock()`.
///
/// Ownership: exactly one Guard owns the lock. Move-only; a default
/// constructed or moved-from Guard holds nothing.
/// Failure: `unlock()` never throws.
/// Thread safety: loop thread only, like the mutex it guards.
class MutexGuard {
public:
    MutexGuard() = default;

    MutexGuard(MutexGuard&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

    MutexGuard& operator=(MutexGuard&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    ~MutexGuard() noexcept { reset(); }

    void unlock() noexcept { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

private:
    friend class detail::AsyncMutexLockAwaitable;

    /// Only the lock awaitable hands one out, so a caller can never manufacture
    /// ownership of a lock it does not hold.
    explicit MutexGuard(detail::MutexState& state) noexcept : state_(&state) {}

    void reset() noexcept {
        if (state_ != nullptr) {
            detail::MutexState* state = std::exchange(state_, nullptr);
            state->release();
        }
    }

    detail::MutexState* state_ = nullptr;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_MUTEX_GUARD_HPP