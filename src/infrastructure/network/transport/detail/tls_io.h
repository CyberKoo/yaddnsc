//
// Direction-correct TLS read and write on an existing SSL session.
//
// SSL_read that reports WANT_WRITE waits for POLLOUT. SSL_write that reports
// WANT_READ waits for POLLIN. The wait keeps the direction from that call.
// A spent deadline does not start another SSL_read or SSL_write, except a
// read of bytes OpenSSL has already decrypted (SSL_pending). That buffer
// belongs to the TLS session, not to the kernel socket.
//

#ifndef YADDNSC_NET_TRANSPORT_DETAIL_TLS_IO_H
#define YADDNSC_NET_TRANSPORT_DETAIL_TLS_IO_H

#include <chrono>
#include <cstddef>
#include <span>
#include <string>

#include <expected>
#include <openssl/ssl.h>

#include "infrastructure/network/transport/io_error.h"

class Socket;

namespace Utils {
class CancellationToken;
}

namespace Transport::detail {

[[nodiscard]] std::string tls_error_text();

/// Wait for the socket direction reported by SSL_get_error.
[[nodiscard]] std::expected<void, IoError> wait_for_tls_direction(int ssl_error, Socket& socket,
                                                                  std::chrono::steady_clock::time_point deadline,
                                                                  const Utils::CancellationToken& token);

/// Read at least one byte. WANT_* retries share @p deadline.
[[nodiscard]] std::expected<std::size_t, IoError> read_ssl(SSL* ssl, Socket& socket, std::span<std::uint8_t> buf,
                                                           std::chrono::steady_clock::time_point deadline,
                                                           const Utils::CancellationToken& token);

/// Write every byte. WANT_* retries share @p deadline.
[[nodiscard]] std::expected<void, IoError> write_ssl(SSL* ssl, Socket& socket, std::span<const std::uint8_t> data,
                                                     std::chrono::steady_clock::time_point deadline,
                                                     const Utils::CancellationToken& token);

}  // namespace Transport::detail

#endif  // YADDNSC_NET_TRANSPORT_DETAIL_TLS_IO_H
