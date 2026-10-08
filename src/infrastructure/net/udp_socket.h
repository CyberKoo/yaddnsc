//
// net — UdpSocket: a datagram socket over the coroutine runtime.
//
// send_to / recv_from are checkpoint-cancellable awaits; the socket itself is
// non-blocking and opened lazily on first use, so an unbound socket picks up an
// ephemeral local port the way a UDP client expects.
//

#ifndef YADDNSC_NET_UDP_SOCKET_H
#define YADDNSC_NET_UDP_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <expected>

#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/datagram.h"
#include "infrastructure/net/io_error.h"
#include "support/util/fd.hpp"

namespace net {

/// A UDP socket.
///
/// The socket is created on first use (bind() or the first send_to/recv_from),
/// non-blocking and close-on-exec. Datagrams larger than the caller's buffer are
/// truncated, not rejected.
///
/// Ownership: one socket object owns one fd, released by close() or destruction.
/// Non-movable, so `this` stays valid for every Task it returns.
/// Failure: expected<T, IoError>. Sending to a target of a different family than
/// the socket is CONNECTION_FAILED.
/// Thread safety: not thread-safe and not concurrent-safe.
class UdpSocket {
public:
    /// @param family  Address family to use when opening the socket. IPV4 (the
    ///                default meaning of UNSPECIFIED) or IPV6.
    explicit UdpSocket(AddressFamily family = AddressFamily::IPV4);

    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) = delete;
    UdpSocket& operator=(UdpSocket&&) = delete;

    /// Bind to a local address and port; port 0 asks for an ephemeral one.
    /// Idempotent: binding an already-bound socket is a no-op. Does not block.
    [[nodiscard]] std::expected<void, IoError> bind(InetAddress local, std::uint16_t port);

    /// Send one datagram to `target`. Opens the socket on first use.
    [[nodiscard]] coro::Task<std::expected<void, IoError>> send_to(InetAddress target, std::uint16_t port,
                                                                   std::span<const std::uint8_t> data);

    /// Receive one datagram into `buf`, reporting the sender.
    [[nodiscard]] coro::Task<std::expected<Datagram, IoError>> recv_from(std::span<std::uint8_t> buf);

    /// Release the socket. Idempotent; never throws.
    void close() noexcept;

    /// The bound local port, or nullopt while the socket is closed/unbound.
    /// Does not block.
    [[nodiscard]] std::optional<std::uint16_t> local_port() const noexcept;

    /// Borrowed descriptor. -1 when closed.
    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }

private:
    /// Open (and non-block-ify) the socket unless it already exists.
    [[nodiscard]] std::expected<void, IoError> ensure_open();

    AddressFamily family_;
    Utils::UniqueFd fd_;
    bool bound_ = false;
};

}  // namespace net

#endif  // YADDNSC_NET_UDP_SOCKET_H
