//
// TcpStream — self-managing plain TCP byte stream (Transport).
//
#include "infrastructure/network/transport/tcp_stream.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Transport {

TcpStream::TcpStream(std::string host, const std::uint16_t port, Options opts, Utils::CancellationToken token)
    : socket_(std::move(host), port, opts, std::move(token)), opts_(std::move(opts)) {}

std::expected<void, IoError> TcpStream::ensure_connected() {
    if (socket_.is_connected() && socket_.is_healthy()) {
        return {};
    }
    socket_.close();
    return socket_.connect();
}

void TcpStream::close() noexcept {
    socket_.close();
}

std::expected<size_t, IoError> TcpStream::read_some(const std::span<std::uint8_t> buf) {
    if (!socket_.is_connected()) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    if (buf.empty()) {
        return 0;
    }
    return read_once(buf);
}

std::expected<size_t, IoError> TcpStream::read_once(const std::span<std::uint8_t> buf) {
    using enum IoError;

    for (;;) {
        if (auto ready = socket_.poll(POLLIN, opts_.read_timeout); !ready) {
            return std::unexpected(ready.error());
        }

        const ssize_t n = ::recv(socket_.fd(), buf.data(), buf.size(), 0);
        if (n > 0) {
            return static_cast<size_t>(n);
        }
        if (n == 0) {
            return std::unexpected(CONNECTION_FAILED);  // EOF
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;  // spurious readiness — poll again
        }
        SPDLOG_DEBUG("TCP recv failed: {}", std::strerror(errno));
        return std::unexpected(CONNECTION_FAILED);
    }
}

std::expected<void, IoError> TcpStream::read_exact(const std::span<std::uint8_t> buf) {
    auto remaining = buf;
    while (!remaining.empty()) {
        auto n = read_once(remaining);
        if (!n) {
            return std::unexpected(n.error());
        }
        remaining = remaining.subspan(*n);
    }
    return {};
}

std::expected<void, IoError> TcpStream::send_all(const std::span<const std::uint8_t> data) {
    using enum IoError;

    if (!socket_.is_connected()) {
        return std::unexpected(CONNECTION_FAILED);
    }

    auto remaining = data;
    while (!remaining.empty()) {
        if (auto ready = socket_.poll(POLLOUT, opts_.write_timeout); !ready) {
            return std::unexpected(ready.error());
        }

        // kNoSigpipe carries MSG_NOSIGNAL where available; otherwise
        // SO_NOSIGPIPE was set on the socket at connect time.
        const ssize_t n = ::send(socket_.fd(), remaining.data(), remaining.size(), detail::kNoSigpipe);
        if (n > 0) {
            remaining = remaining.subspan(static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;  // spurious readiness — poll again
        }
        SPDLOG_DEBUG("TCP send failed: {}", std::strerror(errno));
        return std::unexpected(CONNECTION_FAILED);
    }
    return {};
}

}  // namespace Transport
