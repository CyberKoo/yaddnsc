//
// TlsStream — self-managing TLS byte stream (Transport).
//
// Evolved from src/infrastructure/network/tls_connection.* (BIO/poll/ssl-ctx internals
// reused), with a redesigned surface: the CancellationToken is bound at
// construction, lifecycle is owned by ensure_connected(), and no method
// takes a token.
//

#ifndef YADDNSC_NET_TRANSPORT_TLS_STREAM_H
#define YADDNSC_NET_TRANSPORT_TLS_STREAM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <expected>
#include <openssl/types.h>
#include <stddef.h>

#include "infrastructure/network/transport/detail/socket_stream.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace Utils {
class CancellationToken;
}  // namespace Utils

namespace Transport {

struct SslContextDeleter {
    void operator()(SSL_CTX* ctx) const noexcept;
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslContextDeleter>;

/// A TLS byte stream over TCP.
///
/// Owns the socket (via detail::SocketStream), the SSL_CTX (shared default
/// or per-instance for custom CA / verification off) and the SSL session.
/// Non-movable: hand out via std::unique_ptr.
class TlsStream final : public Stream {
public:
    /// @throws std::invalid_argument when host is neither a valid IP nor a
    ///         valid domain name (validated eagerly, no I/O).
    TlsStream(std::string host, std::uint16_t port, Options opts, TlsOptions tls_opts, Utils::CancellationToken token);

    ~TlsStream() override;

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    [[nodiscard]] std::expected<void, IoError> ensure_connected() override;
    void close() noexcept override;

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf) override;
    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf) override;
    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data) override;

private:
    [[nodiscard]] std::expected<void, IoError> connect(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] std::expected<void, IoError> handshake(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool is_healthy() const noexcept;

    /// Single SSL_read attempt: poll-aware, returns bytes read (>= 1).
    [[nodiscard]] std::expected<size_t, IoError> read_once(std::span<std::uint8_t> buf);

    /// Active SSL_CTX: per-instance custom ctx when configured, otherwise
    /// the shared default (verify-on, discovered CA).
    [[nodiscard]] SSL_CTX* ssl_ctx() const noexcept;

    detail::SocketStream socket_;
    SslCtxPtr custom_ctx_;
    SSL* ssl_ = nullptr;
    Options opts_;
    TlsOptions tls_opts_;
    std::vector<unsigned char> alpn_proto_;
};

}  // namespace Transport

#endif  // YADDNSC_NET_TRANSPORT_TLS_STREAM_H
