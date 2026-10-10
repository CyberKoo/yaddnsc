//
// net — internal socket primitives shared by the transport streams.
//
// Recoverable transport failures use error values. Cancellation throws
// coro::Cancelled; deadlines are represented by the caller's ScopeOutcome.
//

#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_SOCKET_OPS_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_SOCKET_OPS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <expected>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/network/transport/datagram.h"
#include "infrastructure/network/transport/io_error.h"
#include "support/util/fd.hpp"

namespace domain {
enum class AddressFamily;
}  // namespace domain

namespace net::detail {

/// Open a non-blocking, close-on-exec TCP socket matching @p target's family,
/// with SIGPIPE suppression applied (per send where the platform supports it).
[[nodiscard]] std::expected<Utils::UniqueFd, IoError> open_tcp_socket(const domain::InetAddress& target) noexcept;

/// Open a non-blocking, close-on-exec UDP socket of @p family.
[[nodiscard]] std::expected<Utils::UniqueFd, IoError> open_udp_socket(domain::AddressFamily family) noexcept;

/// Bind to an outbound interface (SO_BINDTODEVICE). A no-op where unsupported.
[[nodiscard]] std::expected<void, IoError> bind_to_interface(int fd, std::string_view interface);

/// Disable Nagle. Best-effort: a failure leaves the connection usable.
[[nodiscard]] std::expected<void, IoError> set_tcp_nodelay(int fd) noexcept;

/// Bind to a local address/port; port 0 asks the kernel for an ephemeral one.
/// Does not block.
[[nodiscard]] std::expected<void, IoError> bind_local(int fd, const domain::InetAddress& local,
                                                      std::uint16_t port) noexcept;

/// setsockopt with raw option data — multicast membership, IGMP/MLD interface,
/// TTL/hops, and the other option shapes the transport helpers do not cover.
/// Non-blocking. A failure is CONNECTION_FAILED; the errno is not reported.
[[nodiscard]] std::expected<void, IoError> set_socket_option(int fd, int level, int option, const void* data,
                                                             std::size_t size) noexcept;

/// The local port of a bound socket, in host byte order. Does not block.
[[nodiscard]] std::expected<std::uint16_t, IoError> local_port(int fd) noexcept;

/// Non-blocking connect: EINPROGRESS parks on writability, then SO_ERROR
/// decides. @p fd is borrowed; the caller owns it.
[[nodiscard]] coro::Task<std::expected<void, IoError>> connect_socket(int fd, const domain::InetAddress& target,
                                                                      std::uint16_t port);

/// One recv(). Returns 0 at EOF; parks on readability when the socket would
/// block.
[[nodiscard]] coro::Task<std::expected<std::size_t, IoError>> recv_some(int fd, std::span<std::uint8_t> buf);

/// One send(). Parks on writability when the socket would block.
[[nodiscard]] coro::Task<std::expected<std::size_t, IoError>> send_some(int fd, std::span<const std::uint8_t> data);

/// Fill @p buf completely. A short read is EOF and fails the call.
[[nodiscard]] coro::Task<std::expected<void, IoError>> recv_exact(int fd, std::span<std::uint8_t> buf);

/// Write every byte of @p data.
[[nodiscard]] coro::Task<std::expected<void, IoError>> send_all(int fd, std::span<const std::uint8_t> data);

/// One recvfrom(). Truncates rather than rejects an oversized datagram.
[[nodiscard]] coro::Task<std::expected<Datagram, IoError>> recv_datagram(int fd, std::span<std::uint8_t> buf);

/// One sendto().
[[nodiscard]] coro::Task<std::expected<void, IoError>> send_datagram(int fd, const domain::InetAddress& target,
                                                                     std::uint16_t port,
                                                                     std::span<const std::uint8_t> data);

}  // namespace net::detail

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_SOCKET_OPS_H
