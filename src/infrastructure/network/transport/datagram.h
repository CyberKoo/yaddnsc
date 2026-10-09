//
// net — one received datagram.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_DATAGRAM_H
#define YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_DATAGRAM_H

#include <cstddef>
#include <cstdint>

#include "domain/network/inet_address.h"

namespace net {

/// A received datagram: how many bytes landed in the caller's buffer, and who
/// sent them. Datagrams are truncated, never rejected, when they exceed the
/// buffer — `size` is then the buffer's size.
struct Datagram {
    std::size_t size = 0;
    domain::InetAddress from{};
    std::uint16_t port = 0;
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NET_TRANSPORT_DATAGRAM_H
