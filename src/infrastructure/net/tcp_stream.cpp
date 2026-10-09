//
// net — TcpStream implementation.
//

#include "tcp_stream.h"

#include <cerrno>
#include <utility>

#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include "infrastructure/net/detail/socket_ops.h"

namespace net {

TcpStream::TcpStream(InetAddress address, const std::uint16_t port, ConnectOptions options)
    : address_(address), port_(port), options_(std::move(options)) {}

TcpStream::~TcpStream() {
    close();
}

coro::Task<std::expected<void, IoError>> TcpStream::ensure_connected() {
    if (connected_ && is_healthy()) {
        co_return {};
    }
    close();

    auto opened = detail::open_tcp_socket(address_);
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (options_.interface.has_value()) {
        if (auto bound = detail::bind_to_interface(opened->get(), *options_.interface); !bound) {
            SPDLOG_WARN(R"(Failed to bind socket to interface "{}")", *options_.interface);
            co_return std::unexpected(bound.error());
        }
    }

    fd_ = std::move(*opened);
    if (auto connected = co_await detail::connect_socket(fd_.get(), address_, port_); !connected) {
        // A half-open socket is never left behind: the stream stays reusable.
        close();
        co_return std::unexpected(connected.error());
    }
    if (auto nodelay = detail::set_tcp_nodelay(fd_.get()); !nodelay) {
        SPDLOG_WARN("Failed to set TCP_NODELAY; continuing with Nagle enabled");
    }
    connected_ = true;
    co_return {};
}

coro::Task<std::expected<std::size_t, IoError>> TcpStream::read_some(std::span<std::uint8_t> buf) {
    if (buf.empty()) {
        co_return 0;
    }
    if (!fd_) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    auto received = co_await detail::recv_some(fd_.get(), buf);
    if (!received) {
        co_return std::unexpected(received.error());
    }
    if (*received == 0) {
        // A byte stream cannot deliver an empty non-zero read: zero is EOF.
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    co_return *received;
}

coro::Task<std::expected<void, IoError>> TcpStream::read_exact(std::span<std::uint8_t> buf) {
    if (buf.empty()) {
        co_return {};
    }
    if (!fd_) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    co_return co_await detail::recv_exact(fd_.get(), buf);
}

coro::Task<std::expected<void, IoError>> TcpStream::send_all(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        co_return {};
    }
    if (!fd_) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    co_return co_await detail::send_all(fd_.get(), data);
}

void TcpStream::close() noexcept {
    fd_.reset();
    connected_ = false;
}

bool TcpStream::connected() const noexcept {
    return connected_ && is_healthy();
}

bool TcpStream::is_healthy() const noexcept {
    if (!fd_) {
        return false;
    }
    std::uint8_t probe = 0;
    const ssize_t peeked = ::recv(fd_.get(), &probe, sizeof(probe), MSG_PEEK | MSG_DONTWAIT);
    if (peeked > 0) {
        return true;  // data is waiting, so the peer is still there
    }
    if (peeked == 0) {
        return false;  // orderly close
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

}  // namespace net
