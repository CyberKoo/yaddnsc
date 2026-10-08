//
// net — internal socket primitives shared by the transport streams.
//

#include "socket_ops.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include "infrastructure/coro/fd_wait.hpp"
#include "infrastructure/net/socket_addr.h"

#include "config_cmake.h"

namespace net::detail {
namespace {

/// Per-send SIGPIPE suppression where the kernel offers it. Elsewhere the flag
/// is 0 and the socket itself was configured with SO_NOSIGPIPE at open time.
#ifdef HAVE_MSG_NOSIGNAL
constexpr int NO_SIGPIPE = MSG_NOSIGNAL;
#else
constexpr int NO_SIGPIPE = 0;
#endif

[[nodiscard]] constexpr int native_family(const AddressFamily family) noexcept {
    return family == AddressFamily::IPV6 ? AF_INET6 : AF_INET;
}

[[nodiscard]] constexpr bool would_block(const int errnum) noexcept {
    return errnum == EAGAIN || errnum == EWOULDBLOCK;
}

/// Create, harden and non-block-ify a socket. Returns the owning wrapper, so a
/// failure at any step leaves nothing open.
[[nodiscard]] std::expected<Utils::UniqueFd, IoError> open_socket(int family, int type, int protocol) noexcept {
    int raw = -1;
#ifdef SOCK_CLOEXEC
    raw = ::socket(family, type | SOCK_CLOEXEC, protocol);
    if (raw < 0 && errno == EINVAL) {
        raw = ::socket(family, type, protocol);  // kernel without SOCK_CLOEXEC
    }
#else
    raw = ::socket(family, type, protocol);
#endif
    if (raw < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    Utils::UniqueFd fd{raw};

    const int status_flags = ::fcntl(fd.get(), F_GETFL, 0);
    if (status_flags < 0 || ::fcntl(fd.get(), F_SETFL, status_flags | O_NONBLOCK) < 0 ||
        ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

#if defined(SO_NOSIGPIPE) && !defined(HAVE_MSG_NOSIGNAL)
    const int enabled = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
#endif

    return fd;
}

}  // namespace

std::expected<Utils::UniqueFd, IoError> open_tcp_socket(const InetAddress& target) noexcept {
    return open_socket(native_family(target.get_family()), SOCK_STREAM, IPPROTO_TCP);
}

std::expected<Utils::UniqueFd, IoError> open_udp_socket(const AddressFamily family) noexcept {
    return open_socket(native_family(family), SOCK_DGRAM, IPPROTO_UDP);
}

std::expected<void, IoError> bind_to_interface([[maybe_unused]] const int fd, const std::string_view interface) {
    if (interface.empty()) {
        return {};
    }
#ifdef SO_BINDTODEVICE
    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface.data(), static_cast<socklen_t>(interface.size())) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return {};
#else
    // Best-effort on platforms without SO_BINDTODEVICE, matching the legacy
    // transport: the capability is process-wide, so it is reported once.
    static std::once_flag warned;
    std::call_once(warned, [interface] {
        SPDLOG_WARN(R"(Interface binding is not supported on this platform ("{}"), ignoring)", interface);
    });
    return {};
#endif
}

std::expected<void, IoError> set_tcp_nodelay(const int fd) noexcept {
    const int enabled = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return {};
}

std::expected<void, IoError> bind_local(const int fd, const InetAddress& local, const std::uint16_t port) noexcept {
    const auto addr = SocketAddr::from_inet(local, port);
    if (!addr.has_value()) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    int enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::bind(fd, addr->raw(), addr->raw_len()) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return {};
}

std::expected<void, IoError> set_socket_option(const int fd, const int level, const int option, const void* data,
                                               const std::size_t size) noexcept {
    if (::setsockopt(fd, level, option, data, static_cast<socklen_t>(size)) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return {};
}

std::expected<std::uint16_t, IoError> local_port(const int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return SocketAddr::from_raw(reinterpret_cast<const sockaddr*>(&storage), length).port();
}

coro::Task<std::expected<void, IoError>> connect_socket(const int fd, const InetAddress& target,
                                                        const std::uint16_t port) {
    const auto addr = SocketAddr::from_inet(target, port);
    if (!addr.has_value()) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }

    for (;;) {
        if (::connect(fd, addr->raw(), addr->raw_len()) == 0) {
            co_return {};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EINPROGRESS && errno != EALREADY) {
            co_return std::unexpected(IoError::CONNECTION_FAILED);
        }
        break;  // completion is signalled by writability, then SO_ERROR
    }

    if (auto ready = co_await coro::wait_writable(fd); !ready) {
        co_return std::unexpected(IoError::CANCELLED);
    }

    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    if (so_error != 0) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    co_return {};
}

coro::Task<std::expected<std::size_t, IoError>> recv_some(const int fd, const std::span<std::uint8_t> buf) {
    for (;;) {
        const ssize_t received = ::recv(fd, buf.data(), buf.size(), 0);
        if (received >= 0) {
            co_return static_cast<std::size_t>(received);
        }
        if (errno == EINTR) {
            continue;
        }
        if (would_block(errno)) {
            if (auto ready = co_await coro::wait_readable(fd); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("recv failed: {}", std::strerror(errno));
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
}

coro::Task<std::expected<std::size_t, IoError>> send_some(const int fd, const std::span<const std::uint8_t> data) {
    for (;;) {
        const ssize_t sent = ::send(fd, data.data(), data.size(), NO_SIGPIPE);
        if (sent >= 0) {
            co_return static_cast<std::size_t>(sent);
        }
        if (errno == EINTR) {
            continue;
        }
        if (would_block(errno)) {
            if (auto ready = co_await coro::wait_writable(fd); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("send failed: {}", std::strerror(errno));
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
}

coro::Task<std::expected<void, IoError>> recv_exact(const int fd, const std::span<std::uint8_t> buf) {
    auto remaining = buf;
    while (!remaining.empty()) {
        auto received = co_await recv_some(fd, remaining);
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (*received == 0) {
            co_return std::unexpected(IoError::CONNECTION_FAILED);  // EOF mid-message
        }
        remaining = remaining.subspan(*received);
    }
    co_return {};
}

coro::Task<std::expected<void, IoError>> send_all(const int fd, const std::span<const std::uint8_t> data) {
    auto remaining = data;
    while (!remaining.empty()) {
        auto sent = co_await send_some(fd, remaining);
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
        remaining = remaining.subspan(*sent);
    }
    co_return {};
}

coro::Task<std::expected<Datagram, IoError>> recv_datagram(const int fd, const std::span<std::uint8_t> buf) {
    for (;;) {
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        const ssize_t received =
            ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&storage), &length);
        if (received >= 0) {
            const SocketAddr peer = SocketAddr::from_raw(reinterpret_cast<const sockaddr*>(&storage), length);
            const std::optional<InetAddress> from = peer.address();
            co_return Datagram{static_cast<std::size_t>(received), from.value_or(InetAddress{}), peer.port()};
        }
        if (errno == EINTR) {
            continue;
        }
        if (would_block(errno)) {
            if (auto ready = co_await coro::wait_readable(fd); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("recvfrom failed: {}", std::strerror(errno));
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
}

coro::Task<std::expected<void, IoError>> send_datagram(const int fd, const InetAddress& target,
                                                       const std::uint16_t port,
                                                       const std::span<const std::uint8_t> data) {
    const auto addr = SocketAddr::from_inet(target, port);
    if (!addr.has_value()) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    for (;;) {
        const ssize_t sent = ::sendto(fd, data.data(), data.size(), NO_SIGPIPE, addr->raw(), addr->raw_len());
        if (sent >= 0) {
            co_return {};
        }
        if (errno == EINTR) {
            continue;
        }
        if (would_block(errno)) {
            if (auto ready = co_await coro::wait_writable(fd); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("sendto failed: {}", std::strerror(errno));
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
}

}  // namespace net::detail
