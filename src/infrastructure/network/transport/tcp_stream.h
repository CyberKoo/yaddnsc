//
// TcpStream — self-managing plain TCP byte stream (Transport).
//

#ifndef YADDNSC_NET_TRANSPORT_TCP_STREAM_H
#define YADDNSC_NET_TRANSPORT_TCP_STREAM_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <expected>

#include "infrastructure/network/transport/detail/tcp_connection.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace Utils {
class CancellationToken;
}

namespace Transport {

/// A plain-TCP byte stream.
///
/// Owns the connection via detail::TcpConnection. One read_some / read_exact
/// call spends a single read_timeout; one send_all spends a single
/// write_timeout. Partial transfers, EINTR, and EAGAIN do not refresh it.
/// A spent deadline does not send or recv. An empty buffer performs no I/O
/// and succeeds. A non-empty TCP read that sees EOF returns
/// IoError::CONNECTION_FAILED.
///
/// Non-movable: hand out via std::unique_ptr.
class TcpStream final : public Stream {
public:
    /// @throws std::invalid_argument when host is neither a valid IP nor a
    ///         valid domain name (validated eagerly, no I/O).
    TcpStream(std::string host, std::uint16_t port, Options opts);

    ~TcpStream() override = default;

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    [[nodiscard]] std::expected<void, IoError> ensure_connected(const Utils::CancellationToken& token) override;
    void close() noexcept override;

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf,
                                                           const Utils::CancellationToken& token) override;
    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf,
                                                          const Utils::CancellationToken& token) override;
    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data,
                                                        const Utils::CancellationToken& token) override;

private:
    detail::TcpConnection connection_;
};

}  // namespace Transport

#endif  // YADDNSC_NET_TRANSPORT_TCP_STREAM_H
