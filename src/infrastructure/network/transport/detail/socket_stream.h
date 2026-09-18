//
// Internal: connected TCP socket with cancellable, deadline-bounded
// establishment and poll-based readiness waits.
//
// Composed by TlsStream and TcpStream; not part of the public surface.
//

#ifndef YADDNSC_NET_TRANSPORT_DETAIL_SOCKET_STREAM_H
#define YADDNSC_NET_TRANSPORT_DETAIL_SOCKET_STREAM_H

#include <chrono>
#include <cstdint>
#include <string>

#include <expected>
#include <sys/socket.h>

#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "support/util/cancellation_token.hpp"
#include "support/util/fd.hpp"

namespace Transport::detail {

// Per-platform SIGPIPE suppression flags, mirroring src/infrastructure/network/socket.cpp:
// MSG_NOSIGNAL where available (Linux), otherwise 0 (callers set
// SO_NOSIGPIPE on the socket instead).
#ifdef HAVE_MSG_NOSIGNAL
inline constexpr int kNoSigpipe = MSG_NOSIGNAL;
#else
inline constexpr int kNoSigpipe = 0;
#endif

/// Poll a single fd for readiness, honouring the cancellation token.
///
/// Combines the fd and the token's cancel fd into one poll(); returns
/// CANCELLED when the token is or becomes triggered (the latched flag is
/// consulted both before and after poll, so draining by another consumer
/// of the same source cannot lose the signal).
[[nodiscard]] std::expected<void, IoError> poll_fd(int fd,
                                                   short events,
                                                   std::chrono::milliseconds timeout,
                                                   const Utils::CancellationToken& token);

/// A connected (or connectable) TCP socket. Owns the fd.
class SocketStream {
public:
    SocketStream(std::string host, std::uint16_t port, Options opts, Utils::CancellationToken token);

    // Non-movable: SSL objects (TlsStream) reference the underlying fd.
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    /// Resolve, bind to the outbound interface (if configured), and connect.
    /// Bounded by Options::connect_timeout; cancellable at every stage.
    [[nodiscard]] std::expected<void, IoError> connect();

    void close() noexcept;

    [[nodiscard]] bool is_connected() const noexcept { return fd_.get() >= 0; }

    /// EOF-aware health probe: false when the peer has closed the connection.
    [[nodiscard]] bool is_healthy() const noexcept;

    /// Poll the socket for readiness, honouring the construction-time token.
    [[nodiscard]] std::expected<void, IoError> poll(short events, std::chrono::milliseconds timeout) const;

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    [[nodiscard]] const std::string& host() const noexcept { return host_; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] const Options& options() const noexcept { return opts_; }

private:
    [[nodiscard]] std::expected<void, IoError> connect_one(const struct sockaddr* addr,
                                                           socklen_t addr_len,
                                                           std::chrono::steady_clock::time_point deadline);

    std::string host_;
    std::uint16_t port_;
    Options opts_;
    Utils::CancellationToken token_;
    Utils::UniqueFd fd_;
};

}  // namespace Transport::detail

#endif  // YADDNSC_NET_TRANSPORT_DETAIL_SOCKET_STREAM_H
