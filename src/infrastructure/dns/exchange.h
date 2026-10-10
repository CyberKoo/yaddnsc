//
// dns — shared classic wire exchanges over the coroutine transport.
//
// Both the bootstrap resolver and the classic resolver send one query and read
// one answer; they differ only in what they do with a truncated reply, which
// stays with the caller. These helpers own the socket work and nothing else.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H
#define YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H

#include <cstdint>
#include <span>
#include <vector>

#include <expected>

#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "coro/task.hpp"

namespace dns::detail {

/// Send `query` over UDP and return the first answer from `server`.
///
/// A datagram from any other address or port is ignored. Opens an ephemeral
/// socket per call. Cancellation (including a deadline enforced by the caller's
/// cancel scope) surfaces as `coro::Cancelled`; a socket failure is
/// DnsError::CONNECTION.
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_udp(
    domain::InetAddress server, std::uint16_t port, std::span<const std::uint8_t> query);

/// Send `query` over TCP with the two-byte length prefix and return the answer.
///
/// Opens a fresh connection per call. Failures map as above; a length prefix
/// outside the classic limit is DnsError::PARSE.
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_tcp(
    domain::InetAddress server, std::uint16_t port, std::span<const std::uint8_t> query);

}  // namespace dns::detail

#endif  // YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H
