//
// mDNS response filtering shared by MdnsIpSource and its unit tests.
//
#ifndef YADDNSC_IP_SOURCE_MDNS_RESPONSE_H
#define YADDNSC_IP_SOURCE_MDNS_RESPONSE_H

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "domain/network/inet_address.h"
#include "domain/dns/record_kind.h"

namespace Mdns {
    /// Parse one DNS datagram and return matching A/AAAA answers for hostname.
    /// Invalid DNS packets propagate RecordParser's exception to the caller.
    [[nodiscard]] std::vector<InetAddress> parse_response(std::span<const std::uint8_t> packet,
                                                           std::string_view hostname,
                                                           RecordKind type);
}

#endif // YADDNSC_IP_SOURCE_MDNS_RESPONSE_H
