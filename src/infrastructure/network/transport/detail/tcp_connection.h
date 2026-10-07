//
// Internal: TCP connection establishment for Transport streams.
//
// Owns the destination, the connection Options, and one Socket. Resolves
// hostname targets through bootstrap DNS, binds the outbound interface, and
// tries each address inside the caller's deadline. Byte-stream transfer and
// TLS stay with TcpStream / TlsStream; readiness waiting stays on Socket.
//

#ifndef YADDNSC_NET_TRANSPORT_DETAIL_TCP_CONNECTION_H
#define YADDNSC_NET_TRANSPORT_DETAIL_TCP_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <string>

#include <expected>

#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"

namespace Utils {
class CancellationToken;
}

namespace Transport::detail {

/// A TCP connection. The owned Socket stays non-blocking after connect().
///
/// Non-movable: a TlsStream SSL session borrows the underlying fd.
class TcpConnection {
public:
    /// @throws std::invalid_argument when @p host is neither a valid IP nor a
    ///         valid domain name (validated eagerly, no I/O).
    TcpConnection(std::string host, std::uint16_t port, Options opts);

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    /// Resolve (when @p host is not an IP literal), bind, and connect.
    /// Name lookup and every address attempt share @p deadline.
    [[nodiscard]] std::expected<void, IoError> connect(std::chrono::steady_clock::time_point deadline,
                                                       const Utils::CancellationToken& token);

    void close() noexcept;

    [[nodiscard]] bool is_connected() const noexcept { return !socket_.is_closed(); }

    /// False when the peer has closed the connection. A zero-timeout probe:
    /// pending bytes count as healthy, EOF does not.
    [[nodiscard]] bool is_healthy() const noexcept;

    /// Borrowed socket. The reference is valid for the lifetime of this
    /// connection; close() and a failed reconnect drop the fd it names.
    [[nodiscard]] Socket& socket() noexcept { return socket_; }

    [[nodiscard]] const Socket& socket() const noexcept { return socket_; }

    [[nodiscard]] const std::string& host() const noexcept { return host_; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] const Options& options() const noexcept { return opts_; }

private:
    [[nodiscard]] std::expected<void, IoError> connect_one(const SocketAddr& addr,
                                                           std::chrono::steady_clock::time_point deadline,
                                                           const Utils::CancellationToken& token);

    std::string host_;
    std::uint16_t port_;
    Options opts_;
    Socket socket_;
};

}  // namespace Transport::detail

#endif  // YADDNSC_NET_TRANSPORT_DETAIL_TCP_CONNECTION_H
