//
// Created by Kotarou on 2026/7/6.
//

#ifndef YADDNSC_NETWORK_SOCKET_H
#define YADDNSC_NETWORK_SOCKET_H

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <span>

#include <expected>
#include <sys/socket.h>

#include "infrastructure/network/socket_addr.h"
#include "support/util/fd.hpp"

namespace Utils {
class CancellationToken;
}

// ---------------------------------------------------------------------------
// Socket — sole owner of one POSIX socket fd.
//
// The fd is held by Utils::UniqueFd. A default-constructed or moved-from
// Socket is closed (native_handle() == -1). close() drops the fd once and
// does not retry EINTR. One Socket must not be used, moved, or closed
// concurrently with itself; distinct Socket objects are independent.
//
// Creation goes through open() / accept(), which set CLOEXEC and the
// platform SIGPIPE suppression before the Socket is returned. Failures of
// those steps close the fd and return the errno. Callers translate that
// errno at their own boundary.
//
// Single-shot I/O (send_some / recv_some / send_to / recv_from) performs one
// system call and returns its result. A negative result becomes
// unexpected(errno), including EAGAIN, EWOULDBLOCK, and EINTR — this layer
// does not retry and does not loop until a buffer is full. A returned 0 is
// success: TCP callers treat it as EOF, datagram callers treat it as an
// empty datagram.
//
// connect() and wait_until() are bounded by a steady_clock deadline and an
// operation-scoped cancellation token. The token is not stored. EINTR
// recomputes the remaining time. A deadline that has already passed does not
// block and does not start connect(). wait_until() reports the poll revents
// (so POLLIN can arrive together with POLLHUP) or an error: ETIMEDOUT,
// ECANCELED, or EBADF for a closed or invalid fd.
// ---------------------------------------------------------------------------
class Socket {
public:
    /// Closed socket. No fd is acquired.
    Socket() noexcept = default;

    ~Socket() = default;

    Socket(Socket&&) noexcept = default;

    Socket& operator=(Socket&&) noexcept = default;

    Socket(const Socket&) = delete;

    Socket& operator=(const Socket&) = delete;

    /// Create a socket. CLOEXEC is applied atomically where the platform
    /// provides SOCK_CLOEXEC; otherwise fcntl is used and a concurrent
    /// fork/exec can still inherit the fd.
    [[nodiscard]] static std::expected<Socket, int> open(int domain, int type, int protocol = 0) noexcept;

    // ---- Options -----------------------------------------------------------

    template<typename T>
    [[nodiscard]] std::expected<void, int> set_option(int level, int optname, const T& val) const noexcept {
        return set_option_raw(level, optname, &val, sizeof(val));
    }

    /// Raw setsockopt for variable-length values (e.g. SO_BINDTODEVICE).
    [[nodiscard]] std::expected<void, int> set_option_raw(int level, int optname, const void* val,
                                                          socklen_t len) const noexcept;

    template<typename T>
    [[nodiscard]] std::expected<void, int> get_option(int level, int optname, T& val) const noexcept {
        socklen_t len = sizeof(val);
        if (::getsockopt(native_handle(), level, optname, &val, &len) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    [[nodiscard]] std::expected<void, int> set_nonblocking(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_reuseaddr(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_reuseport(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_broadcast(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_keepalive(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_linger(bool enable, int timeout_sec = 0) const noexcept;

    [[nodiscard]] std::expected<void, int> set_ipv6_only(bool enable) const noexcept;

    // ---- Addresses ---------------------------------------------------------

    [[nodiscard]] std::expected<void, int> bind(const SocketAddr& addr) const noexcept;

    [[nodiscard]] std::expected<SocketAddr, int> get_sockname() const noexcept;

    [[nodiscard]] std::expected<SocketAddr, int> get_peername() const noexcept;

    /// Non-blocking connect bounded by @p deadline. On success the socket
    /// stays non-blocking. The result is the SO_ERROR value when the
    /// handshake finishes with one, otherwise the errno from connect or from
    /// waiting (ETIMEDOUT, ECANCELED, EBADF, ...).
    [[nodiscard]] std::expected<void, int> connect(const SocketAddr& addr,
                                                   std::chrono::steady_clock::time_point deadline,
                                                   const Utils::CancellationToken& token) noexcept;

    [[nodiscard]] std::expected<void, int> listen(int backlog = SOMAXCONN) const noexcept;

    /// Accepted sockets receive the same CLOEXEC / SIGPIPE treatment as open().
    [[nodiscard]] std::expected<Socket, int> accept(SocketAddr* addr = nullptr) const noexcept;

    // ---- Single-shot I/O ---------------------------------------------------

    [[nodiscard]] std::expected<std::size_t, int> send_some(std::span<const std::byte> data,
                                                            int flags = 0) const noexcept;

    [[nodiscard]] std::expected<std::size_t, int> recv_some(std::span<std::byte> buf, int flags = 0) const noexcept;

    [[nodiscard]] std::expected<std::size_t, int> send_to(std::span<const std::byte> data, const SocketAddr& dest,
                                                          int flags = 0) const noexcept;

    [[nodiscard]] std::expected<std::size_t, int> recv_from(std::span<std::byte> buf, SocketAddr* src = nullptr,
                                                            int flags = 0) const noexcept;

    // ---- Control -----------------------------------------------------------

    void shutdown(int how) noexcept;

    void shutdown_read() noexcept { shutdown(SHUT_RD); }

    void shutdown_write() noexcept { shutdown(SHUT_WR); }

    void shutdown_both() noexcept { shutdown(SHUT_RDWR); }

    void close() noexcept;

    /// Wait until @p events, an error condition, cancellation, or @p deadline.
    /// Success is the socket's revents, including POLLERR / POLLHUP when the
    /// kernel reports them. The caller decides whether data is still readable.
    [[nodiscard]] std::expected<short, int> wait_until(short events, std::chrono::steady_clock::time_point deadline,
                                                       const Utils::CancellationToken& token) const noexcept;

    /// Borrowed fd for OpenSSL, platform adapters, and tests. Ownership stays
    /// with this Socket; the number is invalid after close() or move.
    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }

    [[nodiscard]] bool is_closed() const noexcept { return !fd_; }

private:
    explicit Socket(Utils::UniqueFd fd) noexcept;

    /// @p atomic_cloexec is true when the fd was created with SOCK_CLOEXEC / accept4.
    [[nodiscard]] static std::expected<Socket, int> adopt(Utils::UniqueFd fd, bool atomic_cloexec) noexcept;

    Utils::UniqueFd fd_;
};

#endif  // YADDNSC_NETWORK_SOCKET_H
