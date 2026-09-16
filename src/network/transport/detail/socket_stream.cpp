//
// Internal: SocketStream — cancellable TCP connection establishment and
// readiness polling.
//
#include "network/transport/detail/socket_stream.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "network/inet_address.h"
#include "util/cancellation_token.hpp"
#include "util/validation.hpp"

#include "config_cmake.h"
#include "fmt.hpp"

namespace Transport::detail {

namespace {

struct AddrInfoDeleter {
    void operator()(addrinfo* p) const noexcept { ::freeaddrinfo(p); }
};

using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

[[nodiscard]] int af_hint(const std::optional<AddressFamily> af) noexcept {
    switch (af.value_or(AddressFamily::UNSPECIFIED)) {
        case AddressFamily::IPV4:
            return AF_INET;
        case AddressFamily::IPV6:
            return AF_INET6;
        default:
            return AF_UNSPEC;
    }
}

}  // namespace

std::expected<void, IoError> poll_fd(const int fd,
                                     const short events,
                                     const std::chrono::milliseconds timeout,
                                     const Utils::CancellationToken& token) {
    using enum IoError;

    // Latched pre-check: a trigger that another consumer already drained
    // from the pipe must still cancel this operation.
    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }

    std::array<pollfd, 2> fds{};
    fds[0].fd = fd;
    fds[0].events = events;

    auto nfds = nfds_t{1};
    if (token) {
        fds[1].fd = token.native_handle();
        fds[1].events = POLLIN;
        nfds = 2;
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

    if (token && (fds[1].revents & POLLIN)) {
        token.drain();
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

SocketStream::SocketStream(std::string host, const std::uint16_t port, Options opts, Utils::CancellationToken token)
    : host_(std::move(host)), port_(port), opts_(std::move(opts)), token_(std::move(token)) {
    // Validate eagerly so the caller gets a clear error at construction time.
    if (!InetAddress::parse(host_).has_value() && !Utils::is_valid_domain(host_)) {
        throw std::invalid_argument(
            fmt::format(R"(Invalid server address: "{}" (not a valid IP or domain name))", host_));
    }
}

std::expected<void, IoError> SocketStream::connect() {
    using enum IoError;

    close();

    if (token_.is_triggered()) {
        return std::unexpected(CANCELLED);
    }

    addrinfo hints{};
    hints.ai_family = af_hint(opts_.address_family);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    AddrInfoPtr res{nullptr};
    if (addrinfo* raw = nullptr; ::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &raw) == 0) {
        res.reset(raw);
    }
    if (!res) {
        SPDLOG_DEBUG(R"(Name resolution failed for "{}")", host_);
        return std::unexpected(CONNECTION_FAILED);
    }

    const auto deadline = std::chrono::steady_clock::now() + opts_.connect_timeout;

    for (auto* ai = res.get(); ai != nullptr; ai = ai->ai_next) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(TIMEOUT);
        }

        if (auto result = connect_one(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), deadline)) {
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
                                                       const std::chrono::steady_clock::time_point deadline) {
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
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto ready = poll_fd(sock.get(), POLLOUT, remaining, token_);
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

std::expected<void, IoError> SocketStream::poll(const short events, const std::chrono::milliseconds timeout) const {
    if (fd_.get() < 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return poll_fd(fd_.get(), events, timeout, token_);
}

}  // namespace Transport::detail
