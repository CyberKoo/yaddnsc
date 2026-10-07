//
// TlsStream — self-managing TLS byte stream (Transport).
//
// Evolved from src/infrastructure/network/tls_connection.* (BIO/poll/ssl-ctx internals
// reused), with lifecycle owned by ensure_connected() and operation-scoped
// cancellation: every blocking method takes the caller's CancellationToken.
//

#ifndef YADDNSC_NET_TRANSPORT_TLS_STREAM_H
#define YADDNSC_NET_TRANSPORT_TLS_STREAM_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <expected>
#include <openssl/types.h>

#include "infrastructure/network/transport/detail/tcp_connection.h"
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

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept;
};

using SslPtr = std::unique_ptr<SSL, SslDeleter>;

/// A TLS byte stream over TCP.
///
/// Owns the TCP connection (via detail::TcpConnection), the SSL_CTX (shared
/// default or per-instance for custom CA / verification off) and the SSL
/// session. One read_some / read_exact spends a single read_timeout; one
/// send_all spends a single write_timeout. WANT_READ / WANT_WRITE wait for
/// that direction and do not refresh the budget. An empty buffer performs
/// no I/O and succeeds.
///
/// Non-movable: hand out via std::unique_ptr.
class TlsStream final : public Stream {
public:
    /// @throws std::invalid_argument when host is neither a valid IP nor a
    ///         valid domain name (validated eagerly, no I/O).
    TlsStream(std::string host, std::uint16_t port, Options opts, TlsOptions tls_opts);

    ~TlsStream() override;

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    [[nodiscard]] std::expected<void, IoError> ensure_connected(const Utils::CancellationToken& token) override;
    void close() noexcept override;

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf,
                                                           const Utils::CancellationToken& token) override;
    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf,
                                                          const Utils::CancellationToken& token) override;
    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data,
                                                        const Utils::CancellationToken& token) override;

private:
    [[nodiscard]] std::expected<void, IoError> connect(std::chrono::steady_clock::time_point deadline,
                                                       const Utils::CancellationToken& token);
    /// TLS handshake on a not-yet-published SSL session (owned by the caller).
    [[nodiscard]] std::expected<void, IoError> handshake(SSL* ssl, std::chrono::steady_clock::time_point deadline,
                                                         const Utils::CancellationToken& token);
    [[nodiscard]] bool is_healthy() const noexcept;

    /// Read at least one byte. @p deadline covers every WANT_* retry.
    [[nodiscard]] std::expected<size_t, IoError> read_once(std::span<std::uint8_t> buf,
                                                           std::chrono::steady_clock::time_point deadline,
                                                           const Utils::CancellationToken& token);

    /// Active SSL_CTX: per-instance custom ctx when configured, otherwise
    /// the shared default (verify-on, discovered CA).
    [[nodiscard]] SSL_CTX* ssl_ctx() const noexcept;

    detail::TcpConnection connection_;
    SslCtxPtr custom_ctx_;
    SslPtr ssl_;
    TlsOptions tls_opts_;
    std::vector<unsigned char> alpn_proto_;
};

}  // namespace Transport

#endif  // YADDNSC_NET_TRANSPORT_TLS_STREAM_H
