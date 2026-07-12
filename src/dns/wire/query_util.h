//
// Created by Kotarou on 2026/7/12.
//

#ifndef YADDNSC_DNS_WIRE_QUERY_UTIL_H
#define YADDNSC_DNS_WIRE_QUERY_UTIL_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "dns/wire/builder.h"
#include "dns/types.h"

namespace DNS {

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
[[nodiscard]] inline std::vector<std::uint8_t> build_query(std::string_view host, RecordType type) {
    return QueryBuilder{}
        .add_question(host, type)
        .add_edns(/*udp_payload_size=*/1232)
        .build();
}

} // namespace DNS

#endif // YADDNSC_DNS_WIRE_QUERY_UTIL_H
