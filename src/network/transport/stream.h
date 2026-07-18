//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_NETWORK_TRANSPORT_STREAM_H
#define YADDNSC_NETWORK_TRANSPORT_STREAM_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Utils {
class CancellationToken;
}

namespace Transport {

/// Errors that can occur during I/O operations on a transport stream.
enum class IoError {
    TIMEOUT,             ///< poll() timed out without completing the I/O.
    CANCELLED,           ///< Cancel fd was signalled (caller should abort).
    CONNECTION_FAILED,   ///< Non-recoverable I/O error (connection lost, etc.).
};

/// Abstract bidirectional byte stream for transport-layer I/O.
///
/// Implementations wrap TlsConnection, TcpSocket, QuicStream, etc.
/// This allows protocol logic (HTTP, DNS over TCP, etc.) to be
/// completely transport-agnostic.
///
/// @par Thread Safety
/// **Not thread-safe.** A Stream object must not be accessed concurrently
/// from multiple threads unless the caller provides external synchronization
/// (e.g. a mutex).  Each I/O operation mutates internal state (read/write
/// positions, buffers), so sharing a Stream without locking is unsafe.
class Stream {
public:
    virtual ~Stream() = default;

    /// Read at least one byte, up to @p buf.size().
    /// @return  Number of bytes actually read on success, or an IoError.
    [[nodiscard]] virtual std::expected<size_t, IoError> read_some(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) = 0;

    /// Read exactly @p buf.size() bytes.
    /// @return  Empty on success, or an IoError.
    [[nodiscard]] virtual std::expected<void, IoError> read_exact(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken &cancel_token) = 0;

    /// Send all bytes in @p data.
    /// @return  Empty on success, or an IoError.
    [[nodiscard]] virtual std::expected<void, IoError> send_all(
        std::span<const std::uint8_t> data,
        const Utils::CancellationToken &cancel_token) = 0;
};

}  // namespace Transport

#endif  // YADDNSC_NETWORK_TRANSPORT_STREAM_H
