#ifndef YADDNSC_INFRASTRUCTURE_DNS_ENDPOINT_H
#define YADDNSC_INFRASTRUCTURE_DNS_ENDPOINT_H

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "domain/config/dns_config.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay
#include "domain/network/inet_address.h"
#include "infrastructure/uri/uri.h"

namespace dns {

/// Parsed endpoint semantics shared by config validation and backend assembly.
/// Owns the URI; literal is populated only for classic DNS. No I/O or logging.
struct ResolverEndpoint {
    enum class Kind { CLASSIC, DOT, DOH };
    Kind kind;
    Uri uri;
    std::uint16_t port;
    std::optional<domain::InetAddress> literal;
};

/// Preserve address diagnostic order, including multiple DoH/DoT defects.
/// Allocation failures propagate. Safe for independent concurrent calls.
[[nodiscard]] std::expected<ResolverEndpoint, std::vector<std::string>> parse_resolver_endpoint(
    const domain::DnsServer& server);

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_ENDPOINT_H
