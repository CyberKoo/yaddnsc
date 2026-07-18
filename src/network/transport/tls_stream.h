//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_NETWORK_TRANSPORT_TLS_STREAM_H
#define YADDNSC_NETWORK_TRANSPORT_TLS_STREAM_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "network/transport/stream.h"

namespace Utils {
class CancellationToken;
}

class TlsConnection;

namespace Transport {

/// Stream adapter that wraps a TlsConnection.
///
/// Maps TlsConnection::IoStatus to Transport::IoError so that
/// transport-agnostic protocol readers can operate over TLS without
/// depending on TlsConnection directly.
class TlsStream final : public Stream {
public:
    explicit TlsStream(TlsConnection &conn) noexcept : conn_(conn) {}

    [[nodiscard]] std::expected<size_t, IoError> read_some(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) override;

    [[nodiscard]] std::expected<void, IoError> read_exact(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) override;

    [[nodiscard]] std::expected<void, IoError> send_all(
        std::span<const std::uint8_t> data,
        const Utils::CancellationToken &cancel_token) override;

private:
    TlsConnection &conn_;
};

}  // namespace Transport

#endif  // YADDNSC_NETWORK_TRANSPORT_TLS_STREAM_H
