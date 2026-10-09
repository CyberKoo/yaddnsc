//
// net — UdpSocket implementation.
//

#include "udp_socket.h"

#include <utility>

#include "infrastructure/network/transport/socket_ops.h"

namespace net {

UdpSocket::UdpSocket(const domain::AddressFamily family) : family_(family) {}

UdpSocket::~UdpSocket() {
    close();
}

std::expected<void, IoError> UdpSocket::ensure_open() {
    if (fd_) {
        return {};
    }
    auto opened = detail::open_udp_socket(family_);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    fd_ = std::move(*opened);
    return {};
}

std::expected<void, IoError> UdpSocket::bind(domain::InetAddress local, const std::uint16_t port) {
    if (auto open = ensure_open(); !open) {
        return open;
    }
    if (bound_) {
        return {};
    }
    if (auto bound = detail::bind_local(fd_.get(), local, port); !bound) {
        return bound;
    }
    bound_ = true;
    return {};
}

coro::Task<std::expected<void, IoError>> UdpSocket::send_to(domain::InetAddress target, const std::uint16_t port,
                                                            const std::span<const std::uint8_t> data) {
    if (auto open = ensure_open(); !open) {
        co_return std::unexpected(open.error());
    }
    co_return co_await detail::send_datagram(fd_.get(), target, port, data);
}

coro::Task<std::expected<Datagram, IoError>> UdpSocket::recv_from(const std::span<std::uint8_t> buf) {
    if (auto open = ensure_open(); !open) {
        co_return std::unexpected(open.error());
    }
    co_return co_await detail::recv_datagram(fd_.get(), buf);
}

void UdpSocket::close() noexcept {
    fd_.reset();
    bound_ = false;
}

std::optional<std::uint16_t> UdpSocket::local_port() const noexcept {
    if (!fd_) {
        return std::nullopt;
    }
    auto port = detail::local_port(fd_.get());
    if (!port) {
        return std::nullopt;
    }
    return *port;
}

}  // namespace net
