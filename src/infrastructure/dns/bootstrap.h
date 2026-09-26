//
// Bootstrap name resolution for the transport layer.
//
// Resolves outbound-connection hostnames through explicit bootstrap DNS
// servers using the self-contained classic resolver — no getaddrinfo, no
// NSS. Lives below the transport layer (yaddnsc_dns_classic) so
// SocketStream can use it without a dependency cycle.
//

#ifndef YADDNSC_DNS_BOOTSTRAP_H
#define YADDNSC_DNS_BOOTSTRAP_H

#include <chrono>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"

namespace Utils {
class CancellationToken;
}

namespace DNS {

/// Resolve a hostname to addresses via the given bootstrap DNS servers.
///
/// Servers are tried in order; the first server that yields any matching
/// address wins. Each query honours the cancellation token and the overall
/// deadline is re-checked before every query. NXDOMAIN stops querying the
/// current server (the name does not exist for any record type) without
/// discarding addresses already collected from it.
///
/// @param host      Hostname to resolve (must not be an IP literal).
/// @param family    Restrict to A (IPV4) or AAAA (IPV6); both when nullopt
///                  or UNSPECIFIED (A is queried first).
/// @param servers   Bootstrap servers; must not be empty.
/// @param deadline  Overall budget, shared with the subsequent connect.
/// @param token     Cancellation token observed by every query.
/// @return          Resolved addresses, or the last error encountered.
[[nodiscard]] std::expected<std::vector<InetAddress>, DnsErrorInfo>
resolve_bootstrap(const std::string& host,
                  std::optional<AddressFamily> family,
                  std::span<const Config::DnsServer> servers,
                  std::chrono::steady_clock::time_point deadline,
                  const Utils::CancellationToken& token);

}  // namespace DNS

#endif  // YADDNSC_DNS_BOOTSTRAP_H
