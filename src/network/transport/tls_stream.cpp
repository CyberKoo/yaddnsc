//
// Created by Kotarou on 2026/7/18.
//
#include "tls_stream.h"

#include "network/tls_connection.h"

namespace Transport {

std::expected<size_t, IoError> TlsStream::read_some(
    std::span<std::uint8_t> buf,
    const Utils::CancellationToken &cancel_token) {
    auto result = conn_.read_some(buf, cancel_token);
    if (!result) {
        switch (result.error()) {
        case TlsConnection::IoStatus::TIMEOUT:
            return std::unexpected(IoError::TIMEOUT);
        case TlsConnection::IoStatus::CANCELLED:
            return std::unexpected(IoError::CANCELLED);
        default:
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }
    return *result;
}

std::expected<void, IoError> TlsStream::read_exact(
    std::span<std::uint8_t> buf,
    const Utils::CancellationToken &cancel_token) {
    auto result = conn_.read_exact(buf, cancel_token);
    if (!result) {
        switch (result.error()) {
        case TlsConnection::IoStatus::TIMEOUT:
            return std::unexpected(IoError::TIMEOUT);
        case TlsConnection::IoStatus::CANCELLED:
            return std::unexpected(IoError::CANCELLED);
        default:
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }
    return {};
}

std::expected<void, IoError> TlsStream::send_all(
    std::span<const std::uint8_t> data,
    const Utils::CancellationToken &cancel_token) {
    auto result = conn_.send_all(data, cancel_token);
    if (!result) {
        switch (result.error()) {
        case TlsConnection::IoStatus::TIMEOUT:
            return std::unexpected(IoError::TIMEOUT);
        case TlsConnection::IoStatus::CANCELLED:
            return std::unexpected(IoError::CANCELLED);
        default:
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }
    return {};
}

} // namespace Transport
