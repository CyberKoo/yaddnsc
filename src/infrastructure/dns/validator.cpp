#include "validator.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <expected>

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
}  // namespace dns::Validator
