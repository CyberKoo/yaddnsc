//
// Internal: SocketStream — cancellable TCP connection establishment and
// readiness polling.
//
#include "infrastructure/network/transport/detail/socket_stream.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap.h"
#include "infrastructure/network/socket_addr.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"
#include "support/util/validation.hpp"

#include "config_cmake.h"

namespace Transport::detail {

std::expected<void, IoError> poll_fd(const int fd,
                                     const short events,
                                     const std::chrono::milliseconds timeout,
                                     const Utils::CancellationToken& token) {
    using enum IoError;

    // Latched pre-check: cancellation remains terminal even if poll() was
    // entered after the source fired.
    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }

    // Cancellation is broadcast to this token's own fd, so no shared signal
    // is consumed by another concurrent waiter.
    std::array<pollfd, 2> fds{};
    fds[0].fd = fd;
    fds[0].events = events;
    const auto cancel_fd = token.native_handle();
    const auto nfds = static_cast<nfds_t>(cancel_fd >= 0 ? 2 : 1);
    if (cancel_fd >= 0) {
        fds[1].fd = cancel_fd;
        fds[1].events = POLLIN;
    }

    int ret;
    do {
        ret = ::poll(fds.data(), nfds, static_cast<int>(timeout.count()));
    } while (ret < 0 && errno == EINTR);

    if (ret == 0) {
        return std::unexpected(TIMEOUT);
    }
    if (ret < 0) {
        return std::unexpected(CONNECTION_FAILED);
    }

    if (cancel_fd >= 0 && fds[1].revents & POLLIN) {
        return std::unexpected(CANCELLED);
    }
    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }

    if (fds[0].revents & events) {
        return {};
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return std::unexpected(CONNECTION_FAILED);
    }
    return std::unexpected(CONNECTION_FAILED);
}

SocketStream::SocketStream(std::string host, const std::uint16_t port, Options opts)
    : host_(std::move(host)), port_(port), opts_(std::move(opts)) {
    // Validate eagerly so the caller gets a clear error at construction time.
    if (!InetAddress::parse(host_).has_value() && !Utils::is_valid_domain(host_)) {
        throw std::invalid_argument(
            fmt::format(R"(Invalid server address: "{}" (not a valid IP or domain name))", host_));
    }
}

std::expected<void, IoError> SocketStream::connect(const Utils::CancellationToken& token) {
    using enum IoError;

    close();

    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }

    // Name resolution shares the connect budget.
    const auto deadline = std::chrono::steady_clock::now() + opts_.connect_timeout;

    // Resolve the target into socket addresses. getaddrinfo/NSS is
    // deliberately never used: IP literals connect directly, hostnames go
    // through the configured bootstrap DNS servers (cancellable, bounded
    // by the same deadline).
    std::vector<SocketAddr> addrs;
    if (const auto ip = InetAddress::parse(host_)) {
        if (auto sa = SocketAddr::from_inet(*ip, port_)) {
            addrs.push_back(*sa);
        } else {
            return std::unexpected(CONNECTION_FAILED);
        }
    } else {
        if (opts_.bootstrap_dns.empty()) {
            SPDLOG_WARN(R"(Cannot resolve hostname "{}": no bootstrap DNS servers available. )"
                        R"(Set "bootstrap_dns" in the configuration or populate /etc/resolv.conf, )"
                        R"(or use an IP literal. (/etc/hosts and NSS are not consulted.))",
                        host_);
            return std::unexpected(CONNECTION_FAILED);
        }

        auto resolved = DNS::resolve_bootstrap(host_, opts_.address_family, opts_.bootstrap_dns, deadline, token);
        if (!resolved) {
            if (resolved.error().code == DnsError::CANCELLED) {
                return std::unexpected(CANCELLED);
            }
            if (resolved.error().code == DnsError::RETRY) {
                return std::unexpected(TIMEOUT);
            }
            SPDLOG_DEBUG(R"(Bootstrap name resolution failed for "{}": {})", host_, resolved.error().message);
            return std::unexpected(CONNECTION_FAILED);
        }

        for (const auto& addr : *resolved) {
            if (auto sa = SocketAddr::from_inet(addr, port_)) {
                addrs.push_back(*sa);
            }
        }
        if (addrs.empty()) {
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    for (const auto& sa : addrs) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(TIMEOUT);
        }

        if (auto result = connect_one(sa.raw(), sa.raw_len(), deadline, token)) {
            return {};
        } else {
            if (result.error() == TIMEOUT || result.error() == CANCELLED) {
                return std::unexpected(result.error());
            }
            // Hard failure on this address — try the next one.
        }
    }

    SPDLOG_DEBUG(R"(All connection attempts to "{}:{}" failed)", host_, port_);
    return std::unexpected(CONNECTION_FAILED);
}

std::expected<void, IoError> SocketStream::connect_one(const struct sockaddr* addr,
                                                       const socklen_t addr_len,
                                                       const std::chrono::steady_clock::time_point deadline,
                                                       const Utils::CancellationToken& token) {
    using enum IoError;

    Utils::UniqueFd sock(::socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP));
    if (!sock) {
        return std::unexpected(CONNECTION_FAILED);
    }

#ifdef SO_BINDTODEVICE
    if (opts_.interface.has_value() && !opts_.interface->empty()) {
        if (::setsockopt(sock.get(), SOL_SOCKET, SO_BINDTODEVICE, opts_.interface->c_str(),
                         static_cast<socklen_t>(opts_.interface->size())) != 0) {
            SPDLOG_WARN(R"(Failed to bind socket to interface "{}": {})", *opts_.interface, std::strerror(errno));
            return std::unexpected(CONNECTION_FAILED);
        }
    }
#else
    if (opts_.interface.has_value() && !opts_.interface->empty()) {
        SPDLOG_WARN(R"(Interface binding is not supported on this platform ("{}"), ignoring)", *opts_.interface);
    }
#endif

#ifndef HAVE_MSG_NOSIGNAL
    // Platforms without MSG_NOSIGNAL (macOS / BSD): suppress SIGPIPE per socket.
    const int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    if (const int flags = ::fcntl(sock.get(), F_GETFL, 0);
        flags < 0 || ::fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        return std::unexpected(CONNECTION_FAILED);
    }

    const int rc = ::connect(sock.get(), addr, addr_len);
    if (rc != 0 && errno != EINPROGRESS) {
        return std::unexpected(CONNECTION_FAILED);
    }
    if (rc != 0) {
        // Clamp: a deadline already in the past must not degenerate into an
        // infinite poll() (a negative timeout means "block forever").
        const auto remaining = std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
            std::chrono::milliseconds{0});
        auto ready = poll_fd(sock.get(), POLLOUT, remaining, token);
        if (!ready) {
            return std::unexpected(ready.error());
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    // TCP_NODELAY: disable Nagle — handshakes and request/response exchanges
    // are latency-sensitive.
    const int nodelay = 1;
    if (::setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        SPDLOG_WARN(R"(Failed to set TCP_NODELAY on socket to "{}:{}": {})", host_, port_, std::strerror(errno));
    }

    fd_ = std::move(sock);
    return {};
}

void SocketStream::close() noexcept {
    fd_.reset();
}

bool SocketStream::is_healthy() const noexcept {
    if (fd_.get() < 0) {
        return false;
    }

    pollfd pfd{.fd = fd_.get(), .events = POLLIN, .revents = 0};
    if (::poll(&pfd, 1, 0) <= 0) {
        return true;  // Nothing pending — treat as alive.
    }

    // POLLIN: data or EOF. Peek to distinguish.
    char c;
    const auto n = ::recv(fd_.get(), &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    return n > 0;  // n == 0 means EOF (peer closed).
}

std::expected<void, IoError> SocketStream::poll(const short events,
                                                const std::chrono::milliseconds timeout,
                                                const Utils::CancellationToken& token) const {
    if (fd_.get() < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return poll_fd(fd_.get(), events, timeout, token);
}

}  // namespace Transport::detail
