//
// net — one received datagram.
//

#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_DATAGRAM_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_DATAGRAM_H

#include <cstddef>
#include <cstdint>

#include "domain/network/inet_address.h"

namespace net {

/// A received datagram: how many bytes landed in the caller's buffer, and who
/// sent them. A datagram that exceeds the buffer is truncated by the kernel,
/// never rejected: `size` is then the buffer's size and `truncated` is true.
struct Datagram {
    std::size_t size = 0;
    domain::InetAddress from{};
    std::uint16_t port = 0;
    /// True when the datagram did not fit the caller's buffer: `size` counts
    /// the bytes that landed, the rest was discarded by the kernel.
    bool truncated = false;
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_DATAGRAM_H
