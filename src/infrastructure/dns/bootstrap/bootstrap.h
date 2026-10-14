//
// dns — bootstrap name resolution over the coroutine transport.
//
// Resolves an outbound-connection hostname through explicit bootstrap DNS
// servers: no getaddrinfo, no NSS. Servers are IP literals and raced within the
// policy's concurrency limit; a truncated UDP answer is retried over TCP against the same address.
//
// When the caller expresses no address-family preference, A and AAAA are queried
// concurrently. What the first family to settle reports decides how the other one
// is awaited:
//   * a usable result — the other family then gets a 50ms collection window;
//     unfinished work is cancelled and joined, not left detached;
//   * no usable result — the remaining family is awaited to its own end instead,
//     because its diagnosis may outrank the one already in hand. That wait has no
//     50ms cap: it ends only when that family settles on its own server, UDP
//     budget and TCP fallback deadlines.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H
#define YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H

#include <optional>
#include <string>
#include <vector>

#include <expected>

#include "coro/task.hpp"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/policy.h"  // IWYU pragma: keep — Policy default argument requires the complete type
#include "infrastructure/network/address/host_resolver.h"

namespace domain {
enum class AddressFamily;
struct DnsServer;
}  // namespace domain

namespace dns {

/// Resolve `host` to addresses through the given bootstrap servers.
///
/// @param host     Hostname to resolve; an IP literal bypasses DNS but must match `family`
///                 — a literal of the other family is a local CONFIG mismatch, not a name
///                 without records.
/// @param family   Restrict to one family, or nullopt for both (A first).
/// @param policy   Local stage budgets and bounded server concurrency.
/// @param servers  Bootstrap servers (IP literals); an empty list is a CONFIG
///                 error because /etc/hosts and NSS are never consulted.
/// @return         Addresses collected, or the highest-priority diagnosis:
///                 INVALID_QUERY, CONFIG, PARSE, operational failures, NODATA,
///                 then NXDOMAIN. Endpoint failures/negative answers allow the
///                 next server; invalid local input stops the lookup.
///                 Ancestor cancellation throws `coro::Cancelled`.
/// Failure: malformed packets are translated per attempt; internal defects and
/// allocation failures propagate. Independent calls may run on the loop thread.
[[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> bootstrap_resolve(
    std::string host, std::optional<domain::AddressFamily> family, std::vector<domain::DnsServer> servers,
    Policy policy = {});

/// Bind the explicit bootstrap server policy for injection into HTTP clients.
/// Owns the server list; each returned task owns a copy for its lookup.
[[nodiscard]] net::HostResolver make_bootstrap_resolver(std::vector<domain::DnsServer> servers, Policy policy = {});

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H
