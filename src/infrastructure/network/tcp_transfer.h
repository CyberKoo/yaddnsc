//
// Deadline-bounded TCP byte transfer on an already-connected Socket.
//
// Socket stays single-shot. This is the one full-transfer loop: TcpStream
// and the address-level DNS TCP exchange both call it. A spent deadline
// performs no send and no recv, even when the socket is already ready.
// Kernel readiness is not a buffer this layer is allowed to drain late.
// Empty buffers succeed without I/O. A peer close before read_exact fills
// its buffer is ECONNRESET.
//

#ifndef YADDNSC_NETWORK_TCP_TRANSFER_H
#define YADDNSC_NETWORK_TCP_TRANSFER_H

#include <chrono>
#include <cstddef>
#include <span>

#include <expected>

class Socket;

namespace Utils {
class CancellationToken;
}

[[nodiscard]] std::expected<std::size_t, int> tcp_read_some(Socket& socket, std::span<std::byte> buf,
                                                            std::chrono::steady_clock::time_point deadline,
                                                            const Utils::CancellationToken& token) noexcept;

[[nodiscard]] std::expected<void, int> tcp_read_exact(Socket& socket, std::span<std::byte> buf,
                                                      std::chrono::steady_clock::time_point deadline,
                                                      const Utils::CancellationToken& token) noexcept;

[[nodiscard]] std::expected<void, int> tcp_send_all(Socket& socket, std::span<const std::byte> data,
                                                    std::chrono::steady_clock::time_point deadline,
                                                    const Utils::CancellationToken& token) noexcept;

#endif  // YADDNSC_NETWORK_TCP_TRANSFER_H
