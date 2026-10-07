//
// One classic UDP DNS exchange.
//
// Lives in yaddnsc_dns_classic below the transport layer, beside the TCP
// exchange in classic_tcp.h. Two callers, with different follow-ups:
//   - ClassicResolver (resolver/classic.cpp), above the transport layer,
//     which retries a truncated answer through Transport::TcpStream;
//   - bootstrap.cpp, below it, which retries through classic_tcp.h on the
//     same address.
// Neither this header nor the TCP one decides that; a truncated answer is
// returned as-is and the caller picks the retry.
//

#ifndef YADDNSC_DNS_CLASSIC_UDP_H
#define YADDNSC_DNS_CLASSIC_UDP_H

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include <expected>

#include "domain/error/dns_error_info.h"

class SocketAddr;

namespace Utils {
class CancellationToken;
}  // namespace Utils

namespace DNS {

/// Send @p query to @p server and return the first datagram from that address.
///
/// The send and every discarded datagram share @p deadline. A response from
/// any other source is ignored and does not extend the budget. An empty
/// datagram from the server is returned as an empty buffer. Cancellation is
/// checked before the socket is opened.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> exchange_udp(
    const SocketAddr& server, std::span<const std::uint8_t> query, std::chrono::steady_clock::time_point deadline,
    const Utils::CancellationToken& token, std::uint64_t resolver_id);

}  // namespace DNS

#endif  // YADDNSC_DNS_CLASSIC_UDP_H
