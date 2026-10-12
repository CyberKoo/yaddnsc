//
// dns — shared classic wire exchanges over the coroutine transport.
//
// Bootstrap and Classic share matching, UDP-to-TCP fallback and attempt budgets.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H
#define YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H

#include <cstdint>
#include <span>
#include <vector>

#include <expected>

#include "coro/task.hpp"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/policy.h"

namespace dns::detail {

/// Exchanges own their sockets and copy endpoints; the query bytes are borrowed
/// until the task completes. Independent calls may run concurrently on the loop.

/// One UDP answer: the received bytes, plus whether the datagram was cut off
/// by the receive buffer. Kernel-level truncation does not necessarily set the
/// TC bit, so the caller must fall back to TCP on `truncated` exactly as it
/// would for a TC-bit answer (partial RR bytes must not be parsed).
struct UdpAnswer {
    std::vector<std::uint8_t> bytes;
    bool truncated = false;
};

/// Complete a classic exchange within the policy's attempt budget, including UDP-to-TCP
/// fallback. Both transports must match the query; TC over TCP is a PARSE
/// error. A whole-attempt timeout returns RETRY and identifies UDP or TCP
/// fallback. Used by the Classic backend and bootstrap to keep their policies
/// identical.
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_classic(
    domain::InetAddress server, std::uint16_t port, std::span<const std::uint8_t> query, Policy policy = {});

/// Send `query` over UDP and return the first matching answer from `server`.
///
/// Ignore mismatched source, ID, opcode or question without resetting the
/// policy's wire-exchange budget. Invalid local requests are rejected before
/// I/O; malformed matching responses return PARSE. Periodically yield while
/// discarding floods. Match even a kernel-truncated packet's header/question.
/// Opens an ephemeral
/// socket per call. Cancellation (including a deadline enforced by the caller's
/// cancel scope) surfaces as `coro::Cancelled`; a socket failure is
/// DnsError::CONNECTION.
[[nodiscard]] coro::Task<std::expected<UdpAnswer, domain::DnsErrorInfo>> query_udp(domain::InetAddress server,
                                                                                   std::uint16_t port,
                                                                                   std::span<const std::uint8_t> query,
                                                                                   Policy policy = {});

}  // namespace dns::detail

#endif  // YADDNSC_INFRASTRUCTURE_DNS_EXCHANGE_H
