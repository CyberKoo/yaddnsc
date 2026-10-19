#include "validator.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/types.h"
#include "support/util/bytes.hpp"

namespace dns::Validator {
namespace {
/// The request side of a match: the header checks plus the question that was
/// asked. Both `validate_query` and `match_response` share it so the request is
/// parsed exactly once per call.
[[nodiscard]] std::expected<Question, domain::DnsErrorInfo> parse_request(const std::span<const std::uint8_t> request) {
    if (request.size() < HEADER_SIZE || (request[2] & 0x80) != 0 ||
        Utils::Bytes::read_u16_be(request.subspan(4)) != 1) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY,
                                                    "DNS request has an invalid header or question count"});
    }
    try {
        return RecordParser::parse_question(request);
    } catch (const DnsLookupException& error) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, error.what()});
    }
}
}  // namespace

std::expected<void, domain::DnsErrorInfo> validate_query(const std::span<const std::uint8_t> request) {
    auto asked = parse_request(request);
    if (!asked)
        return std::unexpected(std::move(asked.error()));
    return {};
}

std::expected<bool, domain::DnsErrorInfo> match_response(const std::span<const std::uint8_t> request,
                                                         const std::span<const std::uint8_t> response) {
    auto asked = parse_request(request);
    if (!asked)
        return std::unexpected(std::move(asked.error()));
    return match_response(request, *asked, response);
}

std::expected<bool, domain::DnsErrorInfo> match_response(const std::span<const std::uint8_t> request,
                                                         const Question& asked,
                                                         const std::span<const std::uint8_t> response) {
    // A short packet without an identifiable transaction cannot be attributed
    // to this query. A matching ID, however, must not hide malformed headers.
    if (response.size() < 2 || response[0] != request[0] || response[1] != request[1])
        return false;
    if (response.size() < HEADER_SIZE) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response too short"});
    }
    if ((response[2] & 0x78) != (request[2] & 0x78))
        return false;
    if ((response[2] & 0x80) == 0) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response has QR=0 (not a response)"});
    }
    const auto count = Utils::Bytes::read_u16_be(response.subspan(4));
    if (count == 0 && (response[3] & 0x0f) != 0)
        return true;
    if (count != 1) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response QDCOUNT must be 1"});
    }
    try {
        const auto answered = RecordParser::parse_question(response);
        return name_matches(asked.qname, answered.qname) && asked.qtype == answered.qtype &&
               asked.qclass == answered.qclass;
    } catch (const DnsLookupException& error) {
        return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, std::string("DNS response has malformed question section: ") + error.what()});
    }
}

std::expected<void, domain::DnsErrorInfo> validate_response(const std::span<const std::uint8_t> request,
                                                            const std::span<const std::uint8_t> response) {
    auto matched = match_response(request, response);
    if (!matched)
        return std::unexpected(std::move(matched.error()));
    if (!*matched) {
        return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::PARSE,
                                 "DNS response transaction ID, opcode or question section does not match the query"});
    }
    return {};
}

void select_answers(ParsedMessage& message, const std::span<const std::uint8_t> wire, const std::string& host,
                    const std::optional<RecordType> type_filter) {
    if (host.empty())
        return;
    std::vector<std::string> chain{present_name(host)};
    for (;;) {
        std::optional<std::string> target;
        for (const auto& rr : message.answers) {
            if (rr.qclass != static_cast<std::uint16_t>(RecordClass::IN) ||
                rr.type != static_cast<std::uint16_t>(RecordType::CNAME) || !name_matches(rr.name, chain.back())) {
                continue;
            }
            size_t end = rr.rdata_offset;
            auto next = decompress_name(wire, end);
            const auto length = Utils::Bytes::read_u16_be(wire.subspan(rr.rdata_offset - 2));
            if (end != rr.rdata_offset + length || (target && !name_matches(*target, next))) {
                throw DnsLookupException("Invalid or conflicting CNAME RDATA", domain::DnsError::PARSE);
            }
            target = std::move(next);
        }
        if (!target)
            break;
        // Bound both repeated answer scans and cycle checks on hostile packets.
        // Sixteen alias hops cover ordinary DDNS/CDN names without unbounded work.
        if (chain.size() > 16) {
            throw DnsLookupException("DNS CNAME chain exceeds 16 hops", domain::DnsError::PARSE);
        }
        if (std::ranges::any_of(chain, [&target](const auto& name) { return name_matches(name, *target); })) {
            throw DnsLookupException("DNS CNAME chain contains a cycle", domain::DnsError::PARSE);
        }
        chain.push_back(std::move(*target));
    }
    if (type_filter && std::ranges::none_of(message.answers, [&chain, type_filter](const auto& rr) {
            return rr.qclass == static_cast<std::uint16_t>(RecordClass::IN) &&
                   rr.type == static_cast<std::uint16_t>(*type_filter) && name_matches(rr.name, chain.back());
        })) {
        const auto has_authority = [&message](RecordType type) {
            return std::ranges::any_of(message.authorities, [type](const auto& rr) {
                return rr.qclass == static_cast<std::uint16_t>(RecordClass::IN) &&
                       rr.type == static_cast<std::uint16_t>(type);
            });
        };
        // CNAME-only replies without SOA negative evidence and referrals are
        // unusable endpoint answers, not NODATA: there is no recursive chasing.
        if ((chain.size() > 1 || has_authority(RecordType::NS)) && !has_authority(RecordType::SOA)) {
            throw DnsLookupException("DNS response is CNAME-only or a referral, not a complete answer",
                                     domain::DnsError::PARSE);
        }
    }
    std::erase_if(message.answers, [&chain](const auto& rr) {
        if (rr.qclass != static_cast<std::uint16_t>(RecordClass::IN))
            return true;
        if (rr.type == static_cast<std::uint16_t>(RecordType::CNAME)) {
            return !std::ranges::any_of(chain, [&rr](const auto& name) { return name_matches(rr.name, name); });
        }
        return !name_matches(rr.name, chain.back());
    });
}
}  // namespace dns::Validator
