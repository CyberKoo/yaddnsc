//
// Created by Kotarou on 2026/7/6.
//

#ifndef YADDNSC_NETWORK_SOCKET_H
#define YADDNSC_NETWORK_SOCKET_H

#include <cerrno>
#include <cstddef>
#include <expected>
#include <span>

#include "network/socket_addr.h"

#include "mixin.h"

#include <sys/socket.h>

// ---------------------------------------------------------------------------
// Socket — POSIX socket RAII wrapper.
//
// Policy on exceptions:
//   - Constructor:  throws SocketException on failure (cannot return error code).
//   - I/O (send/recv families):  return ssize_t, do NOT throw.
//   - connect():  returns std::expected<void, ConnectError>, does NOT throw.
//   - Options (set_option/get_option and convenience methods):  return std::expected<void, int>, do NOT throw.
//   - accept:  returns std::expected<Socket, int>, does NOT throw.
//   - Setup/control (bind, set_nonblocking, wait_for):  return std::expected<void, int> or std::expected<int, int>, do NOT throw.
//   - listen:  throw SocketException.
//   - Destructor and close():  noexcept (errors silently ignored).
//
// Thread-safety: a single Socket object must not be used from multiple threads
// simultaneously.  Distinct Socket objects are independent.
// ---------------------------------------------------------------------------
/// Errors that can occur during connect().
enum class ConnectError {
    TIMED_OUT,    ///< Connection timed out (ETIMEDOUT).
    REFUSED,      ///< Connection refused (ECONNREFUSED).
    UNREACHABLE,  ///< Network or host unreachable (ENETUNREACH, EHOSTUNREACH).
    CANCELLED,    ///< Operation cancelled via cancel_fd (ECANCELED).
    INTERNAL,     ///< Internal OS error (fcntl, poll, getsockopt, etc.).
};

class Socket {
public:
    /// Open a new socket.
    /// @throws SocketException on failure.
    explicit Socket(int domain, int type, int protocol = 0);

    ~Socket();

    Socket(Socket &&other) noexcept;

    Socket &operator=(Socket &&other) noexcept;

    // ---- Options: generic setsockopt / getsockopt (type-safe) -------------

    /// Set a socket option.
    /// @return std::expected<void, int> — empty on success, errno on failure.
    template<typename T>
    [[nodiscard]] std::expected<void, int> set_option(int level, int optname, const T &val) const noexcept {
        if (::setsockopt(fd_, level, optname, &val, sizeof(val)) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    /// Raw setsockopt for variable-length values (e.g. SO_BINDTODEVICE).
    [[nodiscard]] std::expected<void, int> set_option_raw(
        int level, int optname, const void *val, socklen_t len) const noexcept;

    /// Get a socket option.
    /// @return std::expected<void, int> — empty on success, errno on failure.
    template<typename T>
    [[nodiscard]] std::expected<void, int> get_option(int level, int optname, T &val) const noexcept {
        socklen_t len = sizeof(val);
        if (::getsockopt(fd_, level, optname, &val, &len) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    /// Toggle O_NONBLOCK via fcntl (not setsockopt).
    [[nodiscard]] std::expected<void, int> set_nonblocking(bool enable) const noexcept;

    // ---- Convenience options (all POSIX portable) -------------------------

    /// Enable/disable SO_REUSEADDR.
    [[nodiscard]] std::expected<void, int> set_reuseaddr(bool enable) const noexcept;

    /// Enable/disable SO_REUSEPORT (Linux 3.9+, BSD).
    /// Returns ENOPROTOOPT on platforms that don't support it.
    [[nodiscard]] std::expected<void, int> set_reuseport(bool enable) const noexcept;

    /// Enable/disable SO_BROADCAST (UDP only).
    [[nodiscard]] std::expected<void, int> set_broadcast(bool enable) const noexcept;

    /// Enable/disable TCP keep-alive probes.
    [[nodiscard]] std::expected<void, int> set_keepalive(bool enable) const noexcept;

    /// Enable/disable SO_LINGER with the given timeout (seconds).
    [[nodiscard]] std::expected<void, int> set_linger(bool enable, int timeout_sec = 0) const noexcept;

    /// Enable/disable IPV6_V6ONLY (RFC 3493).
    [[nodiscard]] std::expected<void, int> set_ipv6_only(bool enable) const noexcept;

    // ---- Address binding: accept SocketAddr instead of raw sockaddr -------

    [[nodiscard]] std::expected<void, int> bind(const SocketAddr &addr) const noexcept;

    [[nodiscard]] SocketAddr get_sockname() const;

    [[nodiscard]] SocketAddr get_peername() const;

    // ---- Connection (client) -----------------------------------------------

    /// Connect to a remote address.
    ///
    /// When @p timeout_sec < 0, performs a blocking connect (with EINTR retry).
    /// When @p timeout_sec >= 0, performs a non-blocking connect with poll()
    /// timeout and restores the original fcntl flags on return.
    ///
    /// Does NOT throw.  Returns OK or a descriptive ConnectError.
    ///
    /// @param addr         Target address.
    /// @param timeout_sec  Timeout in seconds (0 = no wait, negative = block).
    /// @return             std::expected<void, ConnectError>.
    /// NOLINTNEXTLINE(readability-make-member-function-const) — semantically mutates TCP connection state
    [[nodiscard]] std::expected<void, ConnectError> connect(const SocketAddr &addr, int timeout_sec = -1);

    // ---- Listening + accept (server) ---------------------------------------

    void listen(int backlog = SOMAXCONN) const;

    /// Accept an incoming connection.
    ///
    /// @note On Linux, the accepted socket does NOT inherit O_NONBLOCK from the
    ///       listening socket.  If the listener is in non-blocking mode, set
    ///       O_NONBLOCK on the returned socket explicitly via set_nonblocking().
    ///
    /// @warning There is a TOCTOU (time-of-check-time-of-use) race between
    ///          accept() returning and fcntl(FD_CLOEXEC) being set on the new
    ///          descriptor.  In multi-threaded programs this can leak the fd to
    ///          child processes across fork() + exec().  The portable POSIX
    ///          accept() API cannot atomically set CLOEXEC; consider using
    ///          accept4() on platforms that support it (Linux 2.6.28+).
    ///
    /// @param addr  Optional buffer to receive the peer address.
    /// @return      A new Socket representing the accepted connection, or
    ///              std::unexpected(errno) on failure (including EAGAIN).
    [[nodiscard]] std::expected<Socket, int> accept(SocketAddr *addr = nullptr) const noexcept;

    // ---- I/O (all return ssize_t, no exceptions) ---------------------------
    //
    //  Return value convention:
    //      > 0      — bytes transferred
    //        0      — peer closed (TCP only)
    //      < 0      — error, check errno
    //

    /// Send all bytes in @p data (loops internally on short writes).
    /// @return  data.size() on success, -1 on error (errno set).
    [[nodiscard]] ssize_t send(std::span<const std::byte> data) const;

    [[nodiscard]] ssize_t send(std::span<const std::byte> data, int flags) const;

    [[nodiscard]] ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest) const;

    [[nodiscard]] ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest, int flags) const;

    /// Receive up to buf.size() bytes.
    ///
    /// For stream sockets (TCP), a single call may return fewer bytes than
    /// requested — the caller should loop if a complete message is expected.
    /// @see recv_exact
    [[nodiscard]] ssize_t recv(std::span<std::byte> buf) const;

    [[nodiscard]] ssize_t recv(std::span<std::byte> buf, int flags) const;

    [[nodiscard]] ssize_t recv_from(std::span<std::byte> buf, SocketAddr *src = nullptr) const;

    [[nodiscard]] ssize_t recv_from(std::span<std::byte> buf, int flags, SocketAddr *src = nullptr) const;

    /// Receive exactly buf.size() bytes (stream sockets only).
    /// Internally loops until all bytes are obtained.
    /// @return  buf.size() on success.
    ///          A non-negative value less than buf.size() means the peer closed early.
    ///          -1 on error (errno set).
    [[nodiscard]] ssize_t recv_exact(std::span<std::byte> buf) const;

    [[nodiscard]] ssize_t recv_exact(std::span<std::byte> buf, int flags) const;

    /// Send a scatter/gather message (vectored I/O).
    /// @return bytes sent on success, -1 on error (errno set).
    [[nodiscard]] ssize_t sendmsg(const struct msghdr *msg, int flags = 0) const;

    /// Receive a scatter/gather message (vectored I/O).
    /// @return bytes received on success, 0 on peer close, -1 on error (errno set).
    [[nodiscard]] ssize_t recvmsg(struct msghdr *msg, int flags = 0) const;

    // ---- Control -----------------------------------------------------------

    /// Shut down one or both directions (SHUT_RD, SHUT_WR, SHUT_RDWR).
    /// No-op if the socket is already closed (or already shut down).
    /// NOLINTNEXTLINE(readability-make-member-function-const) — semantically mutates TCP connection state
    void shutdown(int how) noexcept;

    /// Shutdown only the read side (SHUT_RD).
    void shutdown_read() noexcept {
        shutdown(SHUT_RD);
    }

    /// Shutdown only the write side (SHUT_WR).
    void shutdown_write() noexcept {
        shutdown(SHUT_WR);
    }

    /// Shutdown both read and write (SHUT_RDWR).
    void shutdown_both() noexcept {
        shutdown(SHUT_RDWR);
    }

    /// Close the socket (idempotent; safe to call multiple times).
    void close() noexcept;

    /// Wait for socket readiness via poll().
    /// When @p cancel_fd >= 0, also monitors that fd for cancellation.
    /// If the cancel fd becomes readable, returns std::unexpected(ECANCELED).
    /// @param events     Events to poll for (POLLIN, POLLOUT, etc.).
    /// @param timeout_ms Timeout in milliseconds.
    /// @param cancel_fd  Optional fd to monitor for cancellation signal.
    /// @return  1 on ready, 0 on timeout (timeout_ms = 0 returns immediately),
    ///          or std::unexpected(errno) / std::unexpected(ECANCELED).
    [[nodiscard]] std::expected<int, int> wait_for(short events, int timeout_ms, int cancel_fd = -1) const noexcept;

    // ---- Accessors ---------------------------------------------------------

    [[nodiscard]] int native_handle() const noexcept {
        return fd_;
    }

    /// True after close(), or if the socket was default-constructed / moved-from.
    [[nodiscard]] bool is_closed() const noexcept {
        return fd_ < 0;
    }

private:
    [[maybe_unused, no_unique_address]] NoCopy no_copy_;
    int fd_{-1};
    int type_{-1};

    /// Private default constructor — only used by accept().
    Socket() noexcept = default;
};

#endif  // YADDNSC_NETWORK_SOCKET_H
