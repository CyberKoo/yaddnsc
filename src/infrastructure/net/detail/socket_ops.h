//
// net — internal socket primitives shared by the transport streams.
//
// Everything here is non-blocking and cancellable: a syscall that would block
// parks the coroutine on the runtime's fd-wait checkpoint instead, and the only
// failure modes it reports are CANCELLED (the enclosing cancel scope fired) and
// CONNECTION_FAILED. There is no deadline parameter: a deadine is a cancel scope
// the caller wraps the operation in.
//

#ifndef YADDNSC_NET_DETAIL_SOCKET_OPS_H
#define YADDNSC_NET_DETAIL_SOCKET_OPS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <expected>

#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/datagram.h"
#include "infrastructure/net/io_error.h"
#include "support/util/fd.hpp"

namespace net::detail {

/// Open a non-blocking, close-on-exec TCP socket matching @p target's family,
/// with SIGPIPE suppression applied (per send where the platform supports it).
[[nodiscard]] std::expected<Utils::UniqueFd, IoError> open_tcp_socket(const InetAddress& target) noexcept;

/// Open a non-blocking, close-on-exec UDP socket of @p family.
[[nodiscard]] std::expected<Utils::UniqueFd, IoError> open_udp_socket(AddressFamily family) noexcept;

/// Bind to an outbound interface (SO_BINDTODEVICE). A no-op where unsupported.
[[nodiscard]] std::expected<void, IoError> bind_to_interface(int fd, std::string_view interface);

/// Disable Nagle. Best-effort: a failure leaves the connection usable.
[[nodiscard]] std::expected<void, IoError> set_tcp_nodelay(int fd) noexcept;

/// Bind to a local address/port; port 0 asks the kernel for an ephemeral one.
/// Does not block.
[[nodiscard]] std::expected<void, IoError> bind_local(int fd, const InetAddress& local, std::uint16_t port) noexcept;

/// The local port of a bound socket, in host byte order. Does not block.
[[nodiscard]] std::expected<std::uint16_t, IoError> local_port(int fd) noexcept;

/// Non-blocking connect: EINPROGRESS parks on writability, then SO_ERROR
/// decides. @p fd is borrowed; the caller owns it.
[[nodiscard]] coro::Task<std::expected<void, IoError>> connect_socket(int fd, const InetAddress& target,
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
[[nodiscard]] coro::Task<std::expected<void, IoError>> send_datagram(int fd, const InetAddress& target,
                                                                     std::uint16_t port,
                                                                     std::span<const std::uint8_t> data);

}  // namespace net::detail

#endif  // YADDNSC_NET_DETAIL_SOCKET_OPS_H
