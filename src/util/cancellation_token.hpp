//
// Created by Kotarou on 2026/7/12.
//

#ifndef YADDNSC_UTIL_CANCELLATION_TOKEN_H
#define YADDNSC_UTIL_CANCELLATION_TOKEN_H

#include "util/fd.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include <unistd.h>

namespace Utils {

namespace detail {

/// Shared state behind CancellationToken / CancellationSource.
///
/// Owning the pipe ends through shared_ptr makes every token self-contained:
/// the fd stays valid as long as any copy of the token exists, so there is
/// no external ordering requirement between the source's lifetime and the
/// consumers' lifetimes.
struct CancellationState {
    explicit CancellationState(std::pair<UniqueFd, UniqueFd> pipe) noexcept
        : read_end(std::move(pipe.first)), write_end(std::move(pipe.second)) {
    }

    UniqueFd read_end;
    UniqueFd write_end;
    std::atomic<bool> triggered{false};
};

} // namespace detail

/// A lightweight, shared-owning token for poll()-based I/O cancellation.
///
/// Semantically analogous to std::stop_token, but designed for poll()-based
/// I/O: when the owning CancellationSource is triggered, a flag latches and
/// the pipe becomes readable, causing poll() to return with POLLIN on
/// native_handle().
///
/// Default-constructed tokens are inert (native_handle() returns -1),
/// eliminating the need for a sentinel value.
///
/// Thread safety: safe to copy and read from any thread.  drain() should
/// be called by the thread that owns the poll()-loop (it modifies the
/// kernel-side pipe buffer, not the token itself).
class CancellationToken {
public:
    CancellationToken() noexcept = default;

    /// The raw read-end fd for use with poll().  Returns -1 when no
    /// cancellation source is associated.  Callers should omit fd -1 from
    /// pollfd arrays.
    [[nodiscard]] int native_handle() const noexcept {
        return state_ ? state_->read_end.get() : -1;
    }

    /// True if this token is linked to an active CancellationSource.
    explicit operator bool() const noexcept { return native_handle() >= 0; }

    /// Latched cancellation state: true once the source was triggered,
    /// even if the pipe signal was already drained by another consumer.
    [[nodiscard]] bool is_triggered() const noexcept {
        return state_ && state_->triggered.load(std::memory_order_acquire);
    }

    /// Drain the cancellation signal from the pipe.
    ///
    /// Must be called after poll() returns POLLIN on this fd to clear the
    /// signal so that subsequent poll() calls on the same pipe do not
    /// spuriously return POLLIN immediately.  The latched triggered flag
    /// is unaffected (see is_triggered()).
    void drain() const noexcept {
        if (state_) {
            std::uint64_t val = 0;
            [[maybe_unused]] auto _ = ::read(state_->read_end.get(), &val, sizeof(val));
        }
    }

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept
        : state_(std::move(state)) {
    }

    std::shared_ptr<detail::CancellationState> state_;

    friend class CancellationSource;
};

/// A source of cancellation that provides shared-owning CancellationTokens.
///
/// trigger() latches the flag and writes a byte to the pipe, making the
/// tokens' fds readable.  Semantically analogous to std::stop_source.
///
/// Thread safety: trigger() is thread-safe and idempotent; it may be called
/// from any thread (main, worker, or signal context).  Tokens may be copied
/// to other threads and remain valid even after the source is destroyed.
class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<detail::CancellationState>(make_pipe())) {
    }

    /// Get a token for this source.
    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

    /// Signal cancellation.  Safe to call from any thread; idempotent.
    void trigger() const noexcept {
        if (!state_) {
            return;
        }
        state_->triggered.store(true, std::memory_order_release);
        if (state_->write_end) {
            std::uint64_t val = 1;
            [[maybe_unused]] auto _ = ::write(state_->write_end.get(), &val, sizeof(val));
        }
    }

    /// Whether trigger() has been called.
    [[nodiscard]] bool is_triggered() const noexcept {
        return state_ && state_->triggered.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace Utils

#endif // YADDNSC_UTIL_CANCELLATION_TOKEN_H
