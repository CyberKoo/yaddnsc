//
// TcpStream — self-managing plain TCP byte stream (Transport).
//
#include "infrastructure/network/transport/tcp_stream.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include "infrastructure/network/tcp_transfer.h"
#include "support/util/cancellation_token.hpp"

namespace Transport {
namespace {

[[nodiscard]] IoError io_error_from_errno(const int errnum, const char* context) {
    if (errnum == ETIMEDOUT) {
        return IoError::TIMEOUT;
    }
    if (errnum == ECANCELED) {
        return IoError::CANCELLED;
    }
    SPDLOG_DEBUG("TCP {} failed: {}", context, std::strerror(errnum));
    return IoError::CONNECTION_FAILED;
}

}  // namespace

TcpStream::TcpStream(std::string host, const std::uint16_t port, Options opts)
    : connection_(std::move(host), port, std::move(opts)) {}

std::expected<void, IoError> TcpStream::ensure_connected(const Utils::CancellationToken& token) {
    if (connection_.is_connected() && connection_.is_healthy()) {
        return {};
    }
    connection_.close();
    const auto deadline = std::chrono::steady_clock::now() + connection_.options().connect_timeout;
    return connection_.connect(deadline, token);
}

void TcpStream::close() noexcept {
    connection_.close();
}

std::expected<size_t, IoError> TcpStream::read_some(const std::span<std::uint8_t> buf,
                                                    const Utils::CancellationToken& token) {
    if (buf.empty()) {
        return 0;
    }
    if (!connection_.is_connected()) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    const auto deadline = std::chrono::steady_clock::now() + connection_.options().read_timeout;
    auto n = tcp_read_some(connection_.socket(), std::as_writable_bytes(buf), deadline, token);
    if (!n) {
        return std::unexpected(io_error_from_errno(n.error(), "recv"));
    }
    // A zero read is EOF. The shared transfer reports it as success so
    // datagram-style callers can tell it from a hard error; a byte stream
    // cannot deliver an empty non-zero read.
    if (*n == 0) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    return *n;
}

std::expected<void, IoError> TcpStream::read_exact(const std::span<std::uint8_t> buf,
                                                   const Utils::CancellationToken& token) {
    if (buf.empty()) {
        return {};
    }
    if (!connection_.is_connected()) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    const auto deadline = std::chrono::steady_clock::now() + connection_.options().read_timeout;
    auto got = tcp_read_exact(connection_.socket(), std::as_writable_bytes(buf), deadline, token);
    if (!got) {
        return std::unexpected(io_error_from_errno(got.error(), "recv"));
    }
    return {};
}

std::expected<void, IoError> TcpStream::send_all(const std::span<const std::uint8_t> data,
                                                 const Utils::CancellationToken& token) {
    if (data.empty()) {
        return {};
    }
    if (!connection_.is_connected()) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    const auto deadline = std::chrono::steady_clock::now() + connection_.options().write_timeout;
    auto sent = tcp_send_all(connection_.socket(), std::as_bytes(data), deadline, token);
    if (!sent) {
        return std::unexpected(io_error_from_errno(sent.error(), "send"));
    }
    return {};
}

}  // namespace Transport
