#ifndef YADDNSC_INFRASTRUCTURE_DNS_WIRE_QUERY_UTIL_H
#define YADDNSC_INFRASTRUCTURE_DNS_WIRE_QUERY_UTIL_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "infrastructure/dns/types.h"
#include "infrastructure/dns/wire/builder.h"

namespace dns {

/// Build a standard DNS query packet with EDNS0 (RFC 6891).
///
/// Convenience wrapper equivalent to:
/// @code
///   QueryBuilder{}
///       .add_question(host, type)
///       .add_edns(/*udp_payload_size=*/1232)
///       .build();
/// @endcode
///
/// The EDNS0 UDP payload size (1232) follows the RECOMMENDED value from
/// RFC 9267 §2 (1220 for IPv4, 1232 for IPv6; 1232 is the common minimum
/// that works for both address families).
///
/// @param host  The domain name to query (e.g. "example.com").
/// @param type  The DNS record type (e.g. RecordType::A, RecordType::AAAA).
/// @return      A buffer containing the raw DNS query packet bytes.
/// @throws DnsPacketException for invalid wire input; Utils::Random::EntropyError
///         when a secure transaction ID cannot be acquired. Backend and bootstrap
///         boundaries distinguish these as INVALID_QUERY and CONNECTION errors.
///         Dispatcher input preflight uses an explicit-ID builder, not this
///         entropy-acquiring helper; secure-ID failure stays inside an attempt.
[[nodiscard]] inline std::vector<std::uint8_t> build_query(std::string_view host, RecordType type) {
    return QueryBuilder{}.add_question(host, type).add_edns(/*udp_payload_size=*/1232).build();
}

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_WIRE_QUERY_UTIL_H
