//
// Created by Kotarou on 2026/7/12.
//

#ifndef YADDNSC_UTIL_CANCELLATION_TOKEN_H
#define YADDNSC_UTIL_CANCELLATION_TOKEN_H

#include "util/fd.hpp"

#include <cstdint>
#include <utility>

#include <poll.h>
#include <unistd.h>

namespace Utils {

/// A lightweight, non-owning token for poll()-based I/O cancellation.
///
/// Wraps the read end of a cancellation pipe. When the corresponding
/// CancellationSource is triggered, the pipe becomes readable, causing
/// poll() to return with POLLIN on this fd.
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

    explicit CancellationToken(int fd) noexcept : fd_(fd) {}

    /// The raw fd for use with poll().  Returns -1 when no cancellation
    /// source is associated.  Callers should omit fd -1 from pollfd arrays.
    [[nodiscard]] int native_handle() const noexcept { return fd_; }

    /// True if this token is linked to an active CancellationSource.
    explicit operator bool() const noexcept { return fd_ >= 0; }

    /// Drain the cancellation signal from the pipe.
    ///
    /// Must be called after poll() returns POLLIN on this fd to clear the
    /// signal so that subsequent poll() calls on the same pipe do not
    /// spuriously return POLLIN immediately.
    void drain() const noexcept {
        if (fd_ >= 0) {
            std::uint64_t val = 0;
            [[maybe_unused]] auto _ = ::read(fd_, &val, sizeof(val));
        }
    }

private:
    int fd_ = -1;
};

/// A source of cancellation that owns a pipe and provides a CancellationToken.
///
/// Semantically analogous to std::stop_source / std::stop_token, but
/// designed for use with poll()-based I/O.  trigger() writes a byte to
/// the pipe, making the token's fd readable.
///
/// Thread safety: trigger() is thread-safe.  The token can be safely
/// copied and passed to other threads.
class CancellationSource {
public:
    CancellationSource() {
        auto [r, w] = make_pipe();
        read_end_ = std::move(r);
        write_end_ = std::move(w);
    }

    /// Get a token for this source.
    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken(read_end_.get());
    }

    /// Signal cancellation.  Safe to call from any thread.
    void trigger() const noexcept {
        if (write_end_) {
            std::uint64_t val = 1;
            [[maybe_unused]] auto _ = ::write(write_end_.get(), &val, sizeof(val));
        }
    }

    /// Whether trigger() has been called.
    [[nodiscard]] bool is_triggered() const noexcept {
        if (!read_end_)
            return false;
        pollfd pfd{read_end_.get(), POLLIN, 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

private:
    UniqueFd read_end_;
    UniqueFd write_end_;
};

} // namespace Utils

#endif // YADDNSC_UTIL_CANCELLATION_TOKEN_H
