//
// Created by Kotarou on 2026/7/6.
//
#include "network/socket.h"

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>

#include <cerrno>
#include <concepts>
#include <cstdint>
#include <utility>

#include "config_cmake.h"
#include "exception/socket.h"

// ===========================================================================
//  SIGPIPE suppression strategy
//
//  When HAVE_MSG_NOSIGNAL is set (detected by CMake), we use MSG_NOSIGNAL
//  on every send/sendto call — this is the cleanest per-call mechanism and
//  is available on Linux.
//
//  On platforms that lack MSG_NOSIGNAL (macOS, iOS, some BSDs), we fall
//  back to the SO_NOSIGPIPE socket option set once during construction.
// ===========================================================================

#ifdef HAVE_MSG_NOSIGNAL
constexpr int YADDNSC_NO_SIGPIPE = MSG_NOSIGNAL;
#else
constexpr int YADDNSC_NO_SIGPIPE = 0;
#endif

// ===========================================================================
//  Local helpers
// ===========================================================================

namespace {
    /// Loop helper for send/sendto.
    ///
    /// For stream sockets (@p stream == true): retries on EINTR and short writes
    /// until all bytes are transferred (TCP semantics).
    /// For datagram sockets (@p stream == false): single-shot send with EINTR
    /// retry only (UDP semantics).
    ///
    /// @return  @p len on success (stream), or the number of bytes sent (datagram),
    ///          -1 on error (errno set).
    template<typename Fn>
        requires requires(Fn f, const std::uint8_t *p, size_t s) {
            { f(p, s) } -> std::convertible_to<ssize_t>;
        }
    [[nodiscard]] ssize_t send_loop(const void *data, size_t len, bool stream, Fn &&fn) {
        auto *buf = static_cast<const std::uint8_t *>(data);
        if (!stream) {
            // Datagram — single-shot (send either succeeds fully or fails).
            ssize_t n;
            do {
                n = fn(buf, len);
            } while (n < 0 && errno == EINTR);
            return n;
        }
        // Stream — loop on short writes.
        size_t total = 0;
        while (total < len) {
            ssize_t n;
            do {
                n = fn(buf + total, len - total);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                return -1;
            }
            total += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(len);
    }

    /// RAII guard that restores the original fcntl flags on destruction.
    /// Used by connect() to ensure O_NONBLOCK is reverted on any exception path.
    class FcntlGuard {
    public:
        FcntlGuard(int fd, int saved_flags) noexcept : fd_(fd), saved_flags_(saved_flags) {
        }

        FcntlGuard(const FcntlGuard &) = delete;
        FcntlGuard(FcntlGuard &&) = delete;

        FcntlGuard &operator=(const FcntlGuard &) = delete;
        FcntlGuard &operator=(FcntlGuard &&) = delete;

        /// Restore flags and mark as disarmed.
        /// @return true on success, false if fcntl failed.
        [[nodiscard]] bool restore() noexcept {
            if (fd_ < 0) return true;
            int rc = ::fcntl(fd_, F_SETFL, saved_flags_);
            fd_ = -1;
            return rc == 0;
        }

        void disarm() noexcept { fd_ = -1; }

        ~FcntlGuard() {
            [[maybe_unused]] auto _ = restore(); // best-effort on exception unwind
        }

    private:
        int fd_;
        int saved_flags_;
    };

    /// Shared implementation for all recv_from overloads.
    [[nodiscard]] ssize_t recv_from_impl(int fd, std::span<std::byte> buf, int flags, SocketAddr *src) noexcept {
        ssize_t n;
        do {
            n = ::recvfrom(
                fd, buf.data(), buf.size(), flags,
                src ? src->raw_mut() : nullptr, src ? src->raw_len_ptr() : nullptr
            );
        } while (n < 0 && errno == EINTR);
        return n;
    }

    /// Translate errno to ConnectError.
    [[nodiscard]] ConnectError to_connect_error(int errnum) noexcept {
        switch (errnum) {
            case ETIMEDOUT:
                return ConnectError::TIMED_OUT;
            case ECONNREFUSED:
                return ConnectError::REFUSED;
            case ENETUNREACH:
            case EHOSTUNREACH:
                return ConnectError::UNREACHABLE;
            case ECANCELED:
                return ConnectError::CANCELLED;
            default:
                return ConnectError::INTERNAL;
        }
    }
} // anonymous namespace

// ===========================================================================
//  Construction / destruction / move
// ===========================================================================

Socket::Socket(int domain, int type, int protocol) : type_(type) {
    fd_ = ::socket(domain, type, protocol);
    if (fd_ < 0) {
        throw SocketException(errno, "socket");
    }

    // Set FD_CLOEXEC to prevent fd leaks to child processes across
    // fork()+exec().  On Linux, prefer socket(AF_*, SOCK_* | SOCK_CLOEXEC, *)
    // for an atomic operation that avoids this TOCTOU race.
    {
        int fd_flags = ::fcntl(fd_, F_GETFD);
        if (fd_flags >= 0) {
            ::fcntl(fd_, F_SETFD, fd_flags | FD_CLOEXEC);
        }
        // If fcntl fails we continue — the socket is still usable.
    }

#ifndef HAVE_MSG_NOSIGNAL
    // Best-effort suppression of SIGPIPE on platforms without MSG_NOSIGNAL.
    // SO_NOSIGPIPE is supported on macOS / FreeBSD / other BSDs.
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket &&other) noexcept : fd_(std::exchange(other.fd_, -1)), type_(other.type_) {
    other.type_ = -1;
}

Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        type_ = std::exchange(other.type_, -1);
    }
    return *this;
}

// ===========================================================================
//  Options
// ===========================================================================

std::expected<void, int> Socket::set_option_raw(int level, int optname, const void *val, socklen_t len) const noexcept {
    if (::setsockopt(fd_, level, optname, val, len) == 0) {
        return {};
    }
    return std::unexpected(errno);
}

std::expected<void, int> Socket::set_nonblocking(bool enable) const noexcept {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(errno);
    }

    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (::fcntl(fd_, F_SETFL, flags) < 0) {
        return std::unexpected(errno);
    }
    return {};
}

std::expected<void, int> Socket::set_reuseaddr(bool enable) const noexcept {
    int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEADDR, val);
}

std::expected<void, int> Socket::set_broadcast(bool enable) const noexcept {
    int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_BROADCAST, val);
}

std::expected<void, int> Socket::set_keepalive(bool enable) const noexcept {
    int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_KEEPALIVE, val);
}

std::expected<void, int> Socket::set_linger(bool enable, int timeout_sec) const noexcept {
    linger l{};
    l.l_onoff = enable ? 1 : 0;
    l.l_linger = static_cast<int>(timeout_sec);
    return set_option(SOL_SOCKET, SO_LINGER, l);
}

std::expected<void, int> Socket::set_ipv6_only(bool enable) const noexcept {
    int val = enable ? 1 : 0;
    return set_option(IPPROTO_IPV6, IPV6_V6ONLY, val);
}

std::expected<void, int> Socket::set_reuseport([[maybe_unused]] bool enable) const noexcept {
#ifdef SO_REUSEPORT
    int val = enable ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEPORT, val);
#else
    return std::unexpected(ENOPROTOOPT);
#endif
}

// ===========================================================================
//  Address binding
// ===========================================================================

std::expected<void, int> Socket::bind(const SocketAddr &addr) const noexcept {
    if (::bind(fd_, addr.raw(), addr.raw_len()) < 0) {
        return std::unexpected(errno);
    }
    return {};
}

SocketAddr Socket::get_sockname() const {
    SocketAddr result;
    if (::getsockname(fd_, result.raw_mut(), result.raw_len_ptr()) < 0) {
        throw SocketException(errno, "getsockname");
    }
    return result;
}

SocketAddr Socket::get_peername() const {
    SocketAddr result;
    if (::getpeername(fd_, result.raw_mut(), result.raw_len_ptr()) < 0) {
        throw SocketException(errno, "getpeername");
    }
    return result;
}

// ===========================================================================
//  Connection
// ===========================================================================

// NOLINTNEXTLINE(readability-make-member-function-const) — see declaration for rationale
std::expected<void, ConnectError> Socket::connect(const SocketAddr &addr, int timeout_sec) {
    if (timeout_sec < 0) {
        // Blocking connect (with EINTR retry).
        int rc;
        do {
            rc = ::connect(fd_, addr.raw(), addr.raw_len());
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            return std::unexpected(to_connect_error(errno));
        }
        return {};
    }

    // Non-blocking connect with poll() timeout.
    // Save original fcntl flags and set O_NONBLOCK; restore on any exit path.
    int orig_flags = ::fcntl(fd_, F_GETFL, 0);
    if (orig_flags < 0) {
        return std::unexpected(ConnectError::INTERNAL);
    }
    if (::fcntl(fd_, F_SETFL, orig_flags | O_NONBLOCK) < 0) {
        return std::unexpected(ConnectError::INTERNAL);
    }
    FcntlGuard guard(fd_, orig_flags);

    int rc;
    do {
        rc = ::connect(fd_, addr.raw(), addr.raw_len());
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        // Connected immediately — restore original flags.
        if (!guard.restore()) {
            return std::unexpected(ConnectError::INTERNAL);
        }
        return {};
    }

    if (errno != EINPROGRESS) {
        return std::unexpected(to_connect_error(errno));
    }

    // Wait for connection to complete.
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;

    do {
        rc = ::poll(&pfd, 1, timeout_sec * 1000);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        if (rc == 0) {
            return std::unexpected(ConnectError::TIMED_OUT);
        }
        return std::unexpected(ConnectError::INTERNAL);
    }

    // Check socket error status.
    int error = 0;
    socklen_t elen = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &elen) < 0) {
        return std::unexpected(ConnectError::INTERNAL);
    }
    if (error != 0) {
        return std::unexpected(to_connect_error(error));
    }

    // Restore original flags.
    if (!guard.restore()) {
        return std::unexpected(ConnectError::INTERNAL);
    }

    return {};
}

// ===========================================================================
//  Listening + accept
// ===========================================================================

void Socket::listen(int backlog) const {
    if (::listen(fd_, backlog) < 0) {
        throw SocketException(errno, "listen");
    }
}

std::expected<Socket, int> Socket::accept(SocketAddr *addr) const noexcept {
    int client_fd;
    if (addr) {
        do {
            client_fd = ::accept(fd_, addr->raw_mut(), addr->raw_len_ptr());
        } while (client_fd < 0 && errno == EINTR);
    } else {
        do {
            client_fd = ::accept(fd_, nullptr, nullptr);
        } while (client_fd < 0 && errno == EINTR);
    }

    if (client_fd < 0) {
        return std::unexpected(errno);
    }

    // Set FD_CLOEXEC (portable POSIX approach; accept4() is Linux-specific).
    //
    // WARNING: There is a TOCTOU race between accept() and fcntl() above:
    // in a multi-threaded process, another thread could fork()+exec() before
    // FD_CLOEXEC is set, leaking this descriptor to the child.  This is an
    // inherent limitation of the POSIX accept() API.  On Linux, accept4()
    // with SOCK_CLOEXEC avoids the race entirely but is non-portable.
    {
        int fd_flags = ::fcntl(client_fd, F_GETFD);
        if (fd_flags >= 0) {
            ::fcntl(client_fd, F_SETFD, fd_flags | FD_CLOEXEC);
        }
    }

    Socket client;
    client.fd_ = client_fd;
    client.type_ = type_;
    return client;
}

// ===========================================================================
//  I/O  —  all take std::span, return ssize_t, no exceptions
// ===========================================================================

ssize_t Socket::send(std::span<const std::byte> data) const {
    return send_loop(
        data.data(), data.size(), type_ == SOCK_STREAM,
        [this](const std::uint8_t *ptr, size_t chunk) {
            return ::send(fd_, ptr, chunk, YADDNSC_NO_SIGPIPE);
        }
    );
}

ssize_t Socket::send(std::span<const std::byte> data, int flags) const {
    return send_loop(
        data.data(), data.size(), type_ == SOCK_STREAM,
        [this, flags](const std::uint8_t *ptr, size_t chunk) {
            return ::send(fd_, ptr, chunk, flags | YADDNSC_NO_SIGPIPE);
        }
    );
}

ssize_t Socket::send_to(std::span<const std::byte> data, const SocketAddr &dest) const {
    return send_loop(
        data.data(), data.size(), type_ == SOCK_STREAM,
        [this, &dest](const std::uint8_t *ptr, size_t chunk) {
            return ::sendto(fd_, ptr, chunk, YADDNSC_NO_SIGPIPE, dest.raw(), dest.raw_len());
        }
    );
}

ssize_t Socket::send_to(std::span<const std::byte> data, const SocketAddr &dest, int flags) const {
    return send_loop(
        data.data(), data.size(), type_ == SOCK_STREAM,
        [this, &dest, flags](const std::uint8_t *ptr, size_t chunk) {
            return ::sendto(fd_, ptr, chunk, flags | YADDNSC_NO_SIGPIPE, dest.raw(), dest.raw_len());
        }
    );
}

ssize_t Socket::recv(std::span<std::byte> buf) const {
    ssize_t n;
    do {
        n = ::recv(fd_, buf.data(), buf.size(), 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t Socket::recv(std::span<std::byte> buf, int flags) const {
    ssize_t n;
    do {
        n = ::recv(fd_, buf.data(), buf.size(), flags);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t Socket::recv_from(std::span<std::byte> buf, SocketAddr *src) const {
    return recv_from_impl(fd_, buf, 0, src);
}

ssize_t Socket::recv_from(std::span<std::byte> buf, int flags, SocketAddr *src) const {
    return recv_from_impl(fd_, buf, flags, src);
}

ssize_t Socket::recv_exact(std::span<std::byte> buf) const {
    return recv_exact(buf, 0);
}

ssize_t Socket::recv_exact(std::span<std::byte> buf, int flags) const {
    if (type_ != SOCK_STREAM) {
        // Datagram socket — single recv() call preserves datagram
        // boundaries.  A loop would merge subsequent datagrams into one
        // buffer, which is always wrong.
        ssize_t n;
        do {
            n = ::recv(fd_, buf.data(), buf.size(), flags);
        } while (n < 0 && errno == EINTR);
        return n;
    }

    // Stream socket — use MSG_WAITALL so the kernel blocks until all
    // requested bytes arrive (or an error/EOF occurs).  This reduces
    // user-space looping overhead.  The manual loop handles any
    // remaining short-read edge cases.
    const int effective_flags = flags | MSG_WAITALL;

    size_t total = 0;
    while (total < buf.size()) {
        ssize_t n;
        do {
            n = ::recv(fd_, buf.data() + total, buf.size() - total, effective_flags);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            return -1; // errno is set
        }
        if (n == 0) {
            return static_cast<ssize_t>(total); // peer closed early
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(buf.size());
}

ssize_t Socket::sendmsg(const msghdr *msg, int flags) const {
    ssize_t n;
    do {
        n = ::sendmsg(fd_, msg, flags | YADDNSC_NO_SIGPIPE);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t Socket::recvmsg(msghdr *msg, int flags) const {
    ssize_t n;
    do {
        n = ::recvmsg(fd_, msg, flags);
    } while (n < 0 && errno == EINTR);
    return n;
}

// ===========================================================================
//  Control
// ===========================================================================

// NOLINTNEXTLINE(readability-make-member-function-const) — see declaration for rationale
void Socket::shutdown(int how) noexcept {
    if (fd_ < 0) {
        return; // already closed — no-op.
    }
    ::shutdown(fd_, how); // silently ignore — socket may already be shut down.
}

void Socket::close() noexcept {
    if (fd_ < 0) {
        return;
    }
    int fd = fd_;
    fd_ = -1; // mark closed immediately to prevent double-close
    // Per POSIX, the state of the fd after close() returns EINTR is
    // unspecified.  Retrying close() risks closing a different fd that
    // another thread may have opened in the meantime.  Call exactly once.
    [[maybe_unused]] auto _ = ::close(fd);
}

std::expected<int, int> Socket::wait_for(short events, int timeout_ms, int cancel_fd) const noexcept {
    pollfd pfds[2];
    pfds[0] = {fd_, events, 0};

    auto nfds = static_cast<nfds_t>(1);
    if (cancel_fd >= 0) {
        pfds[1] = {cancel_fd, POLLIN, 0};
        nfds = static_cast<nfds_t>(2);
    }

    int rc;
    do {
        rc = ::poll(pfds, nfds, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        return std::unexpected(errno);
    }

    // Check cancellation before normal readiness.
    if (nfds > 1 && (pfds[1].revents & POLLIN)) {
        // Drain the cancellation fd (pipe or eventfd) before returning.
        std::uint64_t val = 0;
        [[maybe_unused]] auto _ = ::read(cancel_fd, &val, sizeof(val));
        return std::unexpected(ECANCELED);
    }

    return (pfds[0].revents & events) ? 1 : 0;
}
