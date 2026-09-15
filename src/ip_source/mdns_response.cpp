#include "ip_source/mdns_response.h"

#include <string>

#include "dns/parser.h"
#include "string_util.hpp"

namespace {
    [[nodiscard]] bool name_matches(std::string_view record_name, std::string_view queried) noexcept {
        if (!record_name.empty() && record_name.back() == '.') {
            record_name.remove_suffix(1);
        }
        if (!queried.empty() && queried.back() == '.') {
            queried.remove_suffix(1);
        }
        return StringUtil::iequals(record_name, queried);
    }
}

std::vector<InetAddress> Mdns::parse_response(const std::span<const std::uint8_t> packet,
                                              const std::string_view hostname,
                                              const RecordKind type) {
    DNS::RecordParser parser(packet);
    const auto& msg = parser.message();
    std::vector<InetAddress> results;
    results.reserve(msg.answers.size());

    for (size_t i = 0; i < msg.answers.size(); ++i) {
        const auto& rr = msg.answers[i];
        if (!name_matches(rr.name, hostname)) {
            continue;
        }
        if (type == RecordKind::A && rr.type != static_cast<std::uint16_t>(DNS::RecordType::A)) {
            continue;
        }
        if (type == RecordKind::AAAA && rr.type != static_cast<std::uint16_t>(DNS::RecordType::AAAA)) {
            continue;
        }

        const auto record = parser.parse_record(i);
        if (type == RecordKind::A) {
            if (const auto address = Inet4Address::parse(record)) {
                results.emplace_back(*address);
            }
        } else if (const auto address = Inet6Address::parse(record)) {
            results.emplace_back(*address);
        }
    }
    return results;
}
