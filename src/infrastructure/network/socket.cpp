//
// Created by Kotarou on 2026/7/6.
//
#include "infrastructure/network/socket.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include "support/util/cancellation_token.hpp"

#include "config_cmake.h"

// ===========================================================================
//  SIGPIPE and CLOEXEC
//
//  Linux (and any platform where socket() accepts SOCK_CLOEXEC / accept4
//  accepts SOCK_CLOEXEC) sets close-on-exec atomically. Elsewhere the flag
//  is applied with fcntl after the fd exists. That window is not atomic: a
//  concurrent fork/exec can inherit the descriptor. This build records the
//  fallback in the branch below; it is not a second socket owner.
//
//  SIGPIPE is suppressed per send with MSG_NOSIGNAL when the platform has
//  it. Otherwise SO_NOSIGPIPE is set once, and a failure aborts creation so
//  a later send cannot raise SIGPIPE.
// ===========================================================================

namespace {

#ifdef HAVE_MSG_NOSIGNAL
constexpr int NO_SIGPIPE = MSG_NOSIGNAL;
#else
constexpr int NO_SIGPIPE = 0;
#endif

[[nodiscard]] std::expected<void, int> prepare_fd(const int fd, const bool atomic_cloexec) noexcept {
    if (!atomic_cloexec) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            return std::unexpected(errno);
        }
    }

#ifndef HAVE_MSG_NOSIGNAL
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0) {
        return std::unexpected(errno);
    }
#endif
    return {};
}

/// poll(2) timeout for the time left until @p deadline.
///
/// A deadline that has already passed yields 0 (one non-blocking poll — the
/// caller must not turn that into an infinite wait). A positive remainder
/// rounds up to the next millisecond so a sub-millisecond budget does not
/// collapse to "already due", and is clamped to INT_MAX. The caller loops
/// when the clamp is what elapsed.
[[nodiscard]] int timeout_ms_until(const std::chrono::steady_clock::time_point deadline,
                                   const std::chrono::steady_clock::time_point now) noexcept {
    if (now >= deadline) {
        return 0;
    }
    const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto capped = std::min<std::chrono::milliseconds::rep>(rounded.count(), std::numeric_limits<int>::max());
    return static_cast<int>(capped);
}

[[nodiscard]] std::expected<std::size_t, int> bytes_transferred(const ssize_t n) noexcept {
    if (n < 0) {
        return std::unexpected(errno);
    }
    return static_cast<std::size_t>(n);
}

}  // namespace

Socket::Socket(Utils::UniqueFd fd) noexcept : fd_(std::move(fd)) {}

std::expected<Socket, int> Socket::adopt(Utils::UniqueFd fd, const bool atomic_cloexec) noexcept {
    if (!fd) {
        return std::unexpected(errno);
    }
    if (auto prepared = prepare_fd(fd.get(), atomic_cloexec); !prepared) {
        return std::unexpected(prepared.error());
    }
    return Socket(std::move(fd));
}

std::expected<Socket, int> Socket::open(const int domain, const int type, const int protocol) noexcept {
#if defined(SOCK_CLOEXEC)
    int fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
    bool atomic_cloexec = true;
    if (fd < 0 && errno == EINVAL) {
        // The flag is defined but this kernel rejected it. Fall back; the
        // CLOEXEC window is the non-atomic one documented above.
        fd = ::socket(domain, type, protocol);
        atomic_cloexec = false;
    }
#else
    const int fd = ::socket(domain, type, protocol);
    constexpr bool atomic_cloexec = false;
#endif
    if (fd < 0) {
        return std::unexpected(errno);
    }
    return adopt(Utils::UniqueFd(fd), atomic_cloexec);
}

std::expected<void, int> Socket::set_option_raw(const int level, const int optname, const void* val,
                                                const socklen_t len) const noexcept {
    if (::setsockopt(fd_.get(), level, optname, val, len) == 0) {
        return {};
    }
    return std::unexpected(errno);
}

std::expected<void, int> Socket::set_nonblocking(const bool enable) const noexcept {
    const int flags = ::fcntl(fd_.get(), F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(errno);
    }

    const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd_.get(), F_SETFL, updated) < 0) {
        return std::unexpected(errno);
    }
    return {};
}

std::expected<void, int> Socket::set_reuseaddr(const bool enable) const noexcept {
    const int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEADDR, val);
}

std::expected<void, int> Socket::set_broadcast(const bool enable) const noexcept {
    const int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_BROADCAST, val);
}

std::expected<void, int> Socket::set_keepalive(const bool enable) const noexcept {
    const int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_KEEPALIVE, val);
}

std::expected<void, int> Socket::set_linger(const bool enable, const int timeout_sec) const noexcept {
    linger value{};
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = timeout_sec;
    return set_option(SOL_SOCKET, SO_LINGER, value);
}

std::expected<void, int> Socket::set_ipv6_only(const bool enable) const noexcept {
    const int val = enable ? 1 : 0;
    return set_option(IPPROTO_IPV6, IPV6_V6ONLY, val);
}

std::expected<void, int> Socket::set_reuseport([[maybe_unused]] const bool enable) const noexcept {
#ifdef SO_REUSEPORT
    const int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEPORT, val);
#else
    return std::unexpected(ENOPROTOOPT);
#endif
}

std::expected<void, int> Socket::bind(const SocketAddr& addr) const noexcept {
    if (::bind(fd_.get(), addr.raw(), addr.raw_len()) < 0) {
        return std::unexpected(errno);
    }
    return {};
}

std::expected<SocketAddr, int> Socket::get_sockname() const noexcept {
    SocketAddr result;
    if (::getsockname(fd_.get(), result.raw_mut(), result.raw_len_ptr()) < 0) {
        return std::unexpected(errno);
    }
    return result;
}

std::expected<SocketAddr, int> Socket::get_peername() const noexcept {
    SocketAddr result;
    if (::getpeername(fd_.get(), result.raw_mut(), result.raw_len_ptr()) < 0) {
        return std::unexpected(errno);
    }
    return result;
}

std::expected<void, int> Socket::connect(const SocketAddr& addr, const std::chrono::steady_clock::time_point deadline,
                                         const Utils::CancellationToken& token) noexcept {
    if (is_closed()) {
        return std::unexpected(EBADF);
    }
    if (token.is_triggered()) {
        return std::unexpected(ECANCELED);
    }
    // A spent budget must not start a connect, and must not become poll(-1).
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(ETIMEDOUT);
    }
    if (auto nonblocking = set_nonblocking(true); !nonblocking) {
        return std::unexpected(nonblocking.error());
    }

    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(ECANCELED);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(ETIMEDOUT);
        }

        const int rc = ::connect(fd_.get(), addr.raw(), addr.raw_len());
        if (rc == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINPROGRESS || errno == EALREADY) {
            break;
        }
        return std::unexpected(errno);
    }

    const auto ready = wait_until(POLLOUT, deadline, token);
    if (!ready) {
        return std::unexpected(ready.error());
    }

    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        return std::unexpected(errno);
    }
    if (so_error != 0) {
        return std::unexpected(so_error);
    }
    return {};
}

std::expected<void, int> Socket::listen(const int backlog) const noexcept {
    if (::listen(fd_.get(), backlog) < 0) {
        return std::unexpected(errno);
    }
    return {};
}

std::expected<Socket, int> Socket::accept(SocketAddr* addr) const noexcept {
    if (is_closed()) {
        return std::unexpected(EBADF);
    }

    for (;;) {
#if defined(__linux__)
        const int fd = ::accept4(fd_.get(), addr != nullptr ? addr->raw_mut() : nullptr,
                                 addr != nullptr ? addr->raw_len_ptr() : nullptr, SOCK_CLOEXEC);
        constexpr bool atomic_cloexec = true;
#else
        const int fd = ::accept(fd_.get(), addr != nullptr ? addr->raw_mut() : nullptr,
                                addr != nullptr ? addr->raw_len_ptr() : nullptr);
        constexpr bool atomic_cloexec = false;
#endif
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errno);
        }
        return adopt(Utils::UniqueFd(fd), atomic_cloexec);
    }
}

std::expected<std::size_t, int> Socket::send_some(const std::span<const std::byte> data,
                                                  const int flags) const noexcept {
    const ssize_t n = ::send(fd_.get(), data.data(), data.size(), flags | NO_SIGPIPE);
    return bytes_transferred(n);
}

std::expected<std::size_t, int> Socket::recv_some(const std::span<std::byte> buf, const int flags) const noexcept {
    const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), flags);
    return bytes_transferred(n);
}

std::expected<std::size_t, int> Socket::send_to(const std::span<const std::byte> data, const SocketAddr& dest,
                                                const int flags) const noexcept {
    const ssize_t n = ::sendto(fd_.get(), data.data(), data.size(), flags | NO_SIGPIPE, dest.raw(), dest.raw_len());
    return bytes_transferred(n);
}

std::expected<std::size_t, int> Socket::recv_from(const std::span<std::byte> buf, SocketAddr* src,
                                                  const int flags) const noexcept {
    const ssize_t n = ::recvfrom(fd_.get(), buf.data(), buf.size(), flags, src != nullptr ? src->raw_mut() : nullptr,
                                 src != nullptr ? src->raw_len_ptr() : nullptr);
    return bytes_transferred(n);
}

void Socket::shutdown(const int how) noexcept {
    if (is_closed()) {
        return;
    }
    ::shutdown(fd_.get(), how);
}

void Socket::close() noexcept {
    fd_.reset();
}

std::expected<short, int> Socket::wait_until(const short events, const std::chrono::steady_clock::time_point deadline,
                                             const Utils::CancellationToken& token) const noexcept {
    if (is_closed()) {
        return std::unexpected(EBADF);
    }

    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(ECANCELED);
        }

        const auto now = std::chrono::steady_clock::now();
        const bool due = now >= deadline;
        const int timeout_ms = timeout_ms_until(deadline, now);

        pollfd pfds[2]{};
        pfds[0] = {.fd = fd_.get(), .events = events, .revents = 0};
        const int cancel_fd = token.native_handle();
        auto nfds = nfds_t{1};
        if (cancel_fd >= 0) {
            pfds[1] = {.fd = cancel_fd, .events = POLLIN, .revents = 0};
            nfds = 2;
        }

        const int rc = ::poll(pfds, nfds, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                // The budget is the original deadline. A past deadline must
                // not spin on a stream of EINTR.
                if (std::chrono::steady_clock::now() >= deadline) {
                    return std::unexpected(ETIMEDOUT);
                }
                continue;
            }
            return std::unexpected(errno);
        }

        // Cancellation wins when it is visible alongside socket readiness.
        if (cancel_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return std::unexpected(ECANCELED);
        }
        if (token.is_triggered()) {
            return std::unexpected(ECANCELED);
        }

        if ((pfds[0].revents & POLLNVAL) != 0) {
            return std::unexpected(EBADF);
        }
        if (pfds[0].revents != 0) {
            return pfds[0].revents;
        }
        if (rc == 0) {
            // timeout_ms was clamped to INT_MAX and the real deadline is
            // still ahead: wait for the remainder instead of reporting timeout.
            if (!due && timeout_ms == std::numeric_limits<int>::max() && std::chrono::steady_clock::now() < deadline) {
                continue;
            }
            return std::unexpected(ETIMEDOUT);
        }
        if (due || std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(ETIMEDOUT);
        }
    }
}
