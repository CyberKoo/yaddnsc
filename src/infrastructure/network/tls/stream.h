//
// net — TlsStream: a TLS byte stream over the coroutine runtime.
//
// Owns a TcpStream plus an OpenSSL session. Every SSL_* call that reports
// WANT_READ / WANT_WRITE parks the coroutine on the socket direction OpenSSL
// asked for, so the handshake and the data path are checkpoint-cancellable like
// any other await. No timeout parameter: a deadline is the caller's cancel scope.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_TLS_STREAM_H
#define YADDNSC_INFRASTRUCTURE_NET_TLS_STREAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <expected>
#include <openssl/types.h>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/network/transport/tcp_stream.h"

namespace net {

class TlsContext;

/// Frees an SSL session.
struct SslDeleter {
    void operator()(SSL* ssl) const noexcept;
};

using SslPtr = std::unique_ptr<SSL, SslDeleter>;

/// A TLS byte stream over TCP.
///
/// Targets are already-resolved addresses (see TcpStream). SNI and certificate
/// verification use `TlsOptions::sni_hostname` when set, otherwise the target's
/// IP literal: verification is then against the IP, and no SNI is sent because
/// RFC 6066 §3 forbids an IP literal there.
///
/// The trust material is an immutable TlsContext built off the loop and injected
/// here; the stream never reads a CA bundle. A null context fails closed at the
/// first connect, so the composition root must supply one.
///
/// Ownership: the stream owns the TCP socket and the SSL session, released by
/// close() or destruction (SSL_free before the fd it borrows is closed), and
/// borrows the shared TlsContext.
/// Non-movable, so `this` stays valid for every Task the stream returns.
/// Failure: expected<T, IoError>; a failed connect or handshake closes the
/// socket and leaves the stream reusable. EOF and protocol failures are
/// CONNECTION_FAILED; a cancelled await throws `coro::Cancelled`.
/// Thread safety: not thread-safe and not concurrent-safe.
class TlsStream final : public Stream {
public:
    /// @param address      Destination address (IPv4 or IPv6 literal).
    /// @param port         Destination port, host byte order.
    /// @param context      Pre-built trust context; null fails closed at connect.
    /// @param options      Connection-level options.
    /// @param tls_options  TLS-only options (SNI, ALPN). ALPN bytes are copied.
    TlsStream(domain::InetAddress address, std::uint16_t port, std::shared_ptr<const TlsContext> context,
              ConnectOptions options = {}, TlsOptions tls_options = {});

    ~TlsStream();

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    TlsStream(TlsStream&&) = delete;
    TlsStream& operator=(TlsStream&&) = delete;

    /// Connect the TCP socket and run the handshake. Idempotent: a no-op while
    /// the session is healthy, otherwise the whole connection is rebuilt.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> ensure_connected() override;

    /// Read at least one byte, up to `buf.size()`. EOF is CONNECTION_FAILED; an
    /// empty buffer performs no I/O and succeeds with 0.
    [[nodiscard]] coro::Task<std::expected<std::size_t, IoError>> read_some(std::span<std::uint8_t> buf) override;

    /// Read exactly `buf.size()` bytes. A short read (EOF mid-message) fails.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> read_exact(std::span<std::uint8_t> buf) override;

    /// Write every byte of `data`.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> send_all(std::span<const std::uint8_t> data) override;

    /// Release the session and the socket. Idempotent; never throws.
    void close() noexcept override;

    /// True while the session and its socket are believed good.
    [[nodiscard]] bool connected() const noexcept override;

    /// Borrowed socket descriptor. -1 when closed.
    [[nodiscard]] int native_handle() const noexcept { return tcp_.native_handle(); }

private:
    /// Build the SSL_CTX and SSL session for the current options (no I/O).
    [[nodiscard]] std::expected<void, IoError> prepare_session();

    /// Drive SSL_connect to completion, parking on the requested direction.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> handshake();

    /// "sni-name-or-ip:port" identifying the peer in log lines.
    [[nodiscard]] std::string peer_label() const;

    TcpStream tcp_;
    TlsOptions tls_options_;
    std::vector<unsigned char> alpn_proto_;
    std::shared_ptr<const TlsContext> context_;
    SslPtr ssl_;
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NET_TLS_STREAM_H
