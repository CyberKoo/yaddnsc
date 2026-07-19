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

// ── Forward declarations ──

namespace Utils {
class CancellationToken;
}

#include <sys/socket.h>

// ---------------------------------------------------------------------------
// ConnectError — errors that can occur during connect().
// ---------------------------------------------------------------------------
enum class ConnectError {
    TIMED_OUT,    ///< Connection timed out (ETIMEDOUT).
    REFUSED,      ///< Connection refused (ECONNREFUSED).
    UNREACHABLE,  ///< Network or host unreachable (ENETUNREACH, EHOSTUNREACH).
    CANCELLED,    ///< Operation cancelled via CancellationToken (ECANCELED).
    INTERNAL,     ///< Internal OS error (fcntl, poll, getsockopt, etc.).
};

// ---------------------------------------------------------------------------
// SocketBase — abstract interface for POSIX socket operations.
//
// Template methods (set_option<T>, get_option<T>) are non-virtual and
// delegate to the virtual set_option_raw / a non-virtual implementation.
// Only the I/O and control methods needed for mocking are pure virtual.
// ---------------------------------------------------------------------------
class SocketBase {
public:
    virtual ~SocketBase() = default;

    SocketBase() = default;
    SocketBase(SocketBase &&) noexcept = default;
    SocketBase &operator=(SocketBase &&) noexcept = default;
    SocketBase(const SocketBase &) = delete;
    SocketBase &operator=(const SocketBase &) = delete;

    // ---- Options (non-virtual template delegates to virtual raw) ---------

    template<typename T>
    [[nodiscard]] std::expected<void, int> set_option(int level, int optname, const T &val) const noexcept {
        return set_option_raw(level, optname, &val, sizeof(val));
    }

    /// Raw setsockopt for variable-length values (e.g. SO_BINDTODEVICE).
    [[nodiscard]] virtual std::expected<void, int> set_option_raw(
        int level, int optname, const void *val, socklen_t len) const noexcept = 0;

    template<typename T>
    [[nodiscard]] std::expected<void, int> get_option(int level, int optname, T &val) const noexcept {
        socklen_t len = sizeof(val);
        if (::getsockopt(native_handle(), level, optname, &val, &len) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    [[nodiscard]] virtual std::expected<void, int> set_nonblocking(bool enable) const noexcept = 0;

    // ---- Connection (client) -----------------------------------------------

    [[nodiscard]] virtual std::expected<void, ConnectError> connect(
        const SocketAddr &addr, int timeout_sec = -1) = 0;

    // ---- I/O (all return ssize_t, no exceptions) ---------------------------

    [[nodiscard]] virtual ssize_t send(std::span<const std::byte> data) const = 0;
    [[nodiscard]] virtual ssize_t send(std::span<const std::byte> data, int flags) const = 0;
    [[nodiscard]] virtual ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest) const = 0;
    [[nodiscard]] virtual ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest, int flags) const = 0;

    [[nodiscard]] virtual ssize_t recv(std::span<std::byte> buf) const = 0;
    [[nodiscard]] virtual ssize_t recv(std::span<std::byte> buf, int flags) const = 0;
    [[nodiscard]] virtual ssize_t recv_from(std::span<std::byte> buf, SocketAddr *src = nullptr) const = 0;
    [[nodiscard]] virtual ssize_t recv_from(std::span<std::byte> buf, int flags, SocketAddr *src = nullptr) const = 0;

    [[nodiscard]] virtual ssize_t recv_exact(std::span<std::byte> buf) const = 0;
    [[nodiscard]] virtual ssize_t recv_exact(std::span<std::byte> buf, int flags) const = 0;

    // ---- Control -----------------------------------------------------------

    virtual void shutdown(int how) noexcept = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual std::expected<int, int> wait_for(short events, int timeout_ms) const noexcept = 0;
    [[nodiscard]] virtual std::expected<int, int> wait_for(short events, int timeout_ms,
                                                           const Utils::CancellationToken &cancel_token) const noexcept = 0;

    // ---- Accessors ---------------------------------------------------------

    [[nodiscard]] virtual int native_handle() const noexcept = 0;
    [[nodiscard]] virtual bool is_closed() const noexcept = 0;
};

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
class Socket : public SocketBase {
public:
    /// Open a new socket.
    /// @throws SocketException on failure.
    explicit Socket(int domain, int type, int protocol = 0);

    ~Socket() override;

    Socket(Socket &&other) noexcept;

    Socket &operator=(Socket &&other) noexcept;

    // ---- Options: inherited (set_option<T> via SocketBase) ----------------

    [[nodiscard]] std::expected<void, int> set_option_raw(
        int level, int optname, const void *val, socklen_t len) const noexcept override;

    [[nodiscard]] std::expected<void, int> set_nonblocking(bool enable) const noexcept override;

    // ---- Convenience options (all POSIX portable) -------------------------

    [[nodiscard]] std::expected<void, int> set_reuseaddr(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_reuseport(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_broadcast(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_keepalive(bool enable) const noexcept;

    [[nodiscard]] std::expected<void, int> set_linger(bool enable, int timeout_sec = 0) const noexcept;

    [[nodiscard]] std::expected<void, int> set_ipv6_only(bool enable) const noexcept;

    // ---- Address binding: accept SocketAddr instead of raw sockaddr -------

    [[nodiscard]] std::expected<void, int> bind(const SocketAddr &addr) const noexcept;

    [[nodiscard]] SocketAddr get_sockname() const;

    [[nodiscard]] SocketAddr get_peername() const;

    // ---- Connection (client) -----------------------------------------------

    [[nodiscard]] std::expected<void, ConnectError> connect(
        const SocketAddr &addr, int timeout_sec = -1) override;

    // ---- Listening + accept (server) ---------------------------------------

    void listen(int backlog = SOMAXCONN) const;

    [[nodiscard]] std::expected<Socket, int> accept(SocketAddr *addr = nullptr) const noexcept;

    // ---- I/O (all return ssize_t, no exceptions) ---------------------------

    [[nodiscard]] ssize_t send(std::span<const std::byte> data) const override;

    [[nodiscard]] ssize_t send(std::span<const std::byte> data, int flags) const override;

    [[nodiscard]] ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest) const override;

    [[nodiscard]] ssize_t send_to(std::span<const std::byte> data, const SocketAddr &dest, int flags) const override;

    [[nodiscard]] ssize_t recv(std::span<std::byte> buf) const override;

    [[nodiscard]] ssize_t recv(std::span<std::byte> buf, int flags) const override;

    [[nodiscard]] ssize_t recv_from(std::span<std::byte> buf, SocketAddr *src = nullptr) const override;

    [[nodiscard]] ssize_t recv_from(std::span<std::byte> buf, int flags, SocketAddr *src = nullptr) const override;

    [[nodiscard]] ssize_t recv_exact(std::span<std::byte> buf) const override;

    [[nodiscard]] ssize_t recv_exact(std::span<std::byte> buf, int flags) const override;

    /// Send a scatter/gather message (vectored I/O).
    [[nodiscard]] ssize_t sendmsg(const struct msghdr *msg, int flags = 0) const;

    /// Receive a scatter/gather message (vectored I/O).
    [[nodiscard]] ssize_t recvmsg(struct msghdr *msg, int flags = 0) const;

    // ---- Control -----------------------------------------------------------

    void shutdown(int how) noexcept override;

    void shutdown_read() noexcept {
        shutdown(SHUT_RD);
    }

    void shutdown_write() noexcept {
        shutdown(SHUT_WR);
    }

    void shutdown_both() noexcept {
        shutdown(SHUT_RDWR);
    }

    void close() noexcept override;

    [[nodiscard]] std::expected<int, int> wait_for(short events, int timeout_ms) const noexcept override;

    [[nodiscard]] std::expected<int, int> wait_for(short events, int timeout_ms,
                                                   const Utils::CancellationToken &cancel_token) const noexcept override;

    // ---- Accessors ---------------------------------------------------------

    [[nodiscard]] int native_handle() const noexcept override {
        return fd_;
    }

    [[nodiscard]] bool is_closed() const noexcept override {
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
