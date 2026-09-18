//
// Stream abstraction for Transport — a bidirectional byte stream over
// an established connection (TLS or plain TCP).
//

#ifndef YADDNSC_NET_TRANSPORT_STREAM_H
#define YADDNSC_NET_TRANSPORT_STREAM_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "infrastructure/network/transport/io_error.h"

namespace Utils {
class CancellationToken;
}

namespace Transport {

/// A bidirectional byte stream over an established connection.
///
/// Lifecycle is owned by the implementation: ensure_connected() is
/// idempotent — when already connected and healthy it is a no-op, otherwise
/// the connection is (re)built internally (resolve -> interface bind ->
/// cancellable connect -> TLS handshake).
///
/// Cancellation is operation-scoped: every blocking method takes the
/// caller's CancellationToken (typically derived from the application
/// root), so a single stream can serve operations with different
/// cancellation scopes.  No token is bound at construction.
///
/// Thread safety: **not thread-safe.** A single stream must not be used
/// concurrently from multiple threads; see Session for synchronized reuse.
class Stream {
public:
    virtual ~Stream() = default;

    /// Ensure the connection is established and healthy. Idempotent.
    [[nodiscard]] virtual std::expected<void, IoError>
    ensure_connected(const Utils::CancellationToken& token) = 0;

    /// Close the connection. No-op when not connected.
    virtual void close() noexcept = 0;

    /// Read at least one byte, up to buf.size().
    [[nodiscard]] virtual std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf,
                                                                   const Utils::CancellationToken& token) = 0;

    /// Read exactly buf.size() bytes.
    [[nodiscard]] virtual std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf,
                                                                  const Utils::CancellationToken& token) = 0;

    /// Send all bytes in data.
    [[nodiscard]] virtual std::expected<void, IoError> send_all(std::span<const std::uint8_t> data,
                                                                const Utils::CancellationToken& token) = 0;
};

} // namespace Transport

#endif  // YADDNSC_NET_TRANSPORT_STREAM_H
