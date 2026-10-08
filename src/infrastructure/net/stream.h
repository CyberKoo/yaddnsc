//
// net — the stream abstraction the coroutine clients are written against.
//
// TcpStream and TlsStream implement it, and the HTTP/DNS layers drive their
// protocol code through it, so a test can inject an in-memory stream instead of
// a socket. Every operation is a lazy Task bound to the awaiting loop: no
// reactor, no cancellation token and no timeout parameter anywhere.
//

#ifndef YADDNSC_NET_STREAM_H
#define YADDNSC_NET_STREAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <expected>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/io_error.h"
#include "infrastructure/net/options.h"

namespace net {

/// A connected bidirectional byte stream.
///
/// Ownership: an implementation owns its socket/session; `close()` releases it.
/// Non-movable is the convention, so `this` stays valid for the lifetime of any
/// Task the stream hands out.
/// Failure: every operation returns expected<T, IoError>; the only values are
/// CANCELLED (the enclosing cancel scope fired) and CONNECTION_FAILED (EOF,
/// reset, unusable socket). A stream never reports a timeout.
/// Thread safety: a stream is neither thread-safe nor concurrent-safe: one
/// operation at a time.
class Stream {
public:
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;
    virtual ~Stream() = default;

    /// Connect (or reconnect) the stream. Idempotent while healthy.
    [[nodiscard]] virtual coro::Task<std::expected<void, IoError>> ensure_connected() = 0;

    /// Read at least one byte, up to `buf.size()`. EOF is CONNECTION_FAILED; an
    /// empty buffer performs no I/O and succeeds with 0.
    [[nodiscard]] virtual coro::Task<std::expected<std::size_t, IoError>> read_some(std::span<std::uint8_t> buf) = 0;

    /// Read exactly `buf.size()` bytes; a short read fails.
    [[nodiscard]] virtual coro::Task<std::expected<void, IoError>> read_exact(std::span<std::uint8_t> buf) = 0;

    /// Write every byte of `data`.
    [[nodiscard]] virtual coro::Task<std::expected<void, IoError>> send_all(std::span<const std::uint8_t> data) = 0;

    /// Release the connection. Idempotent; never throws.
    virtual void close() noexcept = 0;

    /// True while the connection is believed good.
    [[nodiscard]] virtual bool connected() const noexcept = 0;
};

/// Creates transport streams, split by intent so a scheme/transport mismatch is
/// impossible: `create_tls` for https, `create_tcp` for http.
///
/// Ownership: returns a stream the caller owns. Tests inject a factory that
/// returns an in-memory stream.
/// Thread safety: implementations must be safe for the loop thread only.
class StreamFactory {
public:
    StreamFactory() = default;
    StreamFactory(const StreamFactory&) = delete;
    StreamFactory& operator=(const StreamFactory&) = delete;
    virtual ~StreamFactory() = default;

    /// Build a TLS stream. May throw std::bad_alloc.
    [[nodiscard]] virtual std::unique_ptr<Stream> create_tls(InetAddress address, std::uint16_t port,
                                                             const ConnectOptions& options,
                                                             const TlsOptions& tls_options) = 0;

    /// Build a plain-TCP stream. May throw std::bad_alloc.
    [[nodiscard]] virtual std::unique_ptr<Stream> create_tcp(InetAddress address, std::uint16_t port,
                                                             const ConnectOptions& options) = 0;
};

/// The production factory: TlsStream for TLS, TcpStream for plain TCP.
class DefaultStreamFactory final : public StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<Stream> create_tls(InetAddress address, std::uint16_t port,
                                                     const ConnectOptions& options,
                                                     const TlsOptions& tls_options) override;

    [[nodiscard]] std::unique_ptr<Stream> create_tcp(InetAddress address, std::uint16_t port,
                                                     const ConnectOptions& options) override;
};

}  // namespace net

#endif  // YADDNSC_NET_STREAM_H
