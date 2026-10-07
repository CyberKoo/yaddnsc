//
// One classic DNS exchange over TCP, to an address that is already resolved.
//
// Lives in yaddnsc_dns_classic beside bootstrap.cpp, which is its caller.
// Bootstrap sits below the transport layer and therefore cannot go through
// TcpStream; this exchange uses the shared Socket transfer directly.
//
// ClassicResolver's own TCP fallback (resolver/classic.cpp) is above the
// transport layer and does use TcpStream. The two paths differ in one
// respect that callers can observe: every step here shares @p deadline,
// while the resolver fallback gives each stream operation its own budget.
//

#ifndef YADDNSC_DNS_CLASSIC_TCP_H
#define YADDNSC_DNS_CLASSIC_TCP_H

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

/// Send @p query to @p server over TCP and return the response body.
///
/// Adds the RFC 1035 two-byte length prefix; this function does not resolve
/// names, so bootstrap can retry a truncated UDP answer here without
/// calling back into TcpConnection.
///
/// Connect, the length-prefixed write, and both reads share @p deadline.
/// A spent deadline does not open a new connection or transfer more bytes.
/// The response length must be in 1..MAX_MESSAGE_SIZE. Cancellation is
/// checked before the socket is opened.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> exchange_tcp(
    const SocketAddr& server, std::span<const std::uint8_t> query, std::chrono::steady_clock::time_point deadline,
    const Utils::CancellationToken& token, std::uint64_t resolver_id);

}  // namespace DNS

#endif  // YADDNSC_DNS_CLASSIC_TCP_H
