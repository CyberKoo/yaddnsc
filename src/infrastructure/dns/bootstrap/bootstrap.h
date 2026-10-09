//
// dns — bootstrap name resolution over the coroutine transport.
//
// Resolves an outbound-connection hostname through explicit bootstrap DNS
// servers: no getaddrinfo, no NSS. Servers are IP literals and are tried in
// order; a truncated UDP answer is retried over TCP against the same address.
//
// When the caller expresses no address-family preference, A and AAAA are queried
// concurrently and each type keeps whatever it finds — the result is the union,
// not the first family to answer.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H
#define YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H

#include <optional>
#include <string>
#include <vector>

#include <expected>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"

namespace dns {

/// Resolve `host` to addresses through the given bootstrap servers.
///
/// @param host     Hostname to resolve; an IP literal is returned as-is.
/// @param family   Restrict to one family, or nullopt for both (A first).
/// @param servers  Bootstrap servers (IP literals); an empty list is a CONFIG
///                 error because /etc/hosts and NSS are never consulted.
/// @return         The addresses found, or the most informative error: a
///                 definitive error (PARSE/CONFIG) is never replaced by a later
///                 transient one, and NXDOMAIN stops the current server.
///                 Cancellation is `coro::Cancelled`.
/// Failure: allocation failures (std::bad_alloc) are rethrown, never downgraded
/// to a retryable error.
[[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> bootstrap_resolve(
    std::string host, std::optional<domain::AddressFamily> family, std::vector<domain::DnsServer> servers);

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_BOOTSTRAP_BOOTSTRAP_H
