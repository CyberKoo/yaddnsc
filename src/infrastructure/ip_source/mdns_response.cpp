#include "infrastructure/ip_source/mdns_response.h"

#include <optional>
#include <string>  // IWYU pragma: keep — rr.name/record are std::string used through conversion; clangd sees no spelled use

#include <stddef.h>
#include "support/string_util.hpp"  // IWYU pragma: keep — the StringUtil namespace alias lives here
#include <yaddnsc/util/string_util.hpp>

#include "domain/dns/record_kind.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/types.h"

std::vector<domain::InetAddress> ipsource::parse_response(const std::span<const std::uint8_t> packet,
                                                          const std::string_view hostname,
                                                          const domain::RecordKind type) {
    dns::RecordParser parser(packet);
    const auto& msg = parser.message();
    std::vector<domain::InetAddress> results;
    if (!msg.qr || !msg.aa || msg.opcode != 0 || msg.rcode != dns::Rcode::NOERROR || msg.tc) {
        return results;
    }
    results.reserve(msg.answers.size());

    for (size_t i = 0; i < msg.answers.size(); ++i) {
        const auto& rr = msg.answers[i];
        // The high CLASS bit is the mDNS cache-flush flag, not part of the class.
        if (rr.ttl == 0 || (rr.qclass & 0x7fff) != static_cast<std::uint16_t>(dns::RecordClass::IN) ||
            !dns::name_matches(rr.name, hostname)) {
            continue;
        }
        if (type == domain::RecordKind::A && rr.type != static_cast<std::uint16_t>(dns::RecordType::A)) {
            continue;
        }
        if (type == domain::RecordKind::AAAA && rr.type != static_cast<std::uint16_t>(dns::RecordType::AAAA)) {
            continue;
        }

        const auto record = parser.parse_record(i);
        if (type == domain::RecordKind::A) {
            if (const auto address = domain::Inet4Address::parse(record)) {
                results.emplace_back(*address);
            }
        } else if (const auto address = domain::Inet6Address::parse(record)) {
            results.emplace_back(*address);
        }
    }
    return results;
}
