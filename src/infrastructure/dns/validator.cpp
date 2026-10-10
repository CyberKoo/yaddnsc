#include "infrastructure/dns/validator.h"

#include <yaddnsc/util/format.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <expected>
#include <string>  // IWYU pragma: keep — fmt::format returns std::string temporaries; clangd sees no spelled use

#include "domain/error/dns_error.h"
#include "infrastructure/dns/types.h"
#include "support/fmt.hpp"
#include "support/util/bytes.hpp"

namespace {
// ── Low-level DNS wire-format helpers ──

/// Fixed size of QTYPE + QCLASS in the question section (RFC 1035 §4.1.2).
constexpr size_t QUESTION_FIXED_SIZE = 4;

/// Skip a DNS wire-format name (QNAME) starting at @p offset.
/// Handles normal labels, compression pointers (0xC0), and the root label (0x00).
/// @return  The offset past the end of the name, or std::nullopt on error.
[[nodiscard]] std::optional<size_t> skip_name(std::span<const std::uint8_t> msg, size_t offset) noexcept {
    while (offset < msg.size()) {
        const auto label_len = msg[offset];
        if (label_len == 0) {
            return offset + 1;  // root label terminates the name
        }
        if ((label_len & 0xC0) == 0xC0) {
            return offset + 2;  // compression pointer — skip 2 bytes
        }
        offset += 1 + label_len;
    }
    return std::nullopt;  // malformed — ran off the end
}

/// Find the end of the first question section, starting past the header.
/// @return  The offset past QNAME + QTYPE + QCLASS, or std::nullopt on error.
[[nodiscard]] std::optional<size_t> question_section_end(std::span<const std::uint8_t> msg) noexcept {
    if (msg.size() < dns::HEADER_SIZE)
        return std::nullopt;
    const auto qdcount = Utils::Bytes::try_read_u16_be(msg, 4);
    if (!qdcount || *qdcount == 0)
        return std::nullopt;
    const auto off = skip_name(msg, dns::HEADER_SIZE);
    if (!off || *off + QUESTION_FIXED_SIZE > msg.size())
        return std::nullopt;
    return *off + QUESTION_FIXED_SIZE;  // skip QTYPE + QCLASS
}

// ── Individual validation checks (all return expected) ──

[[nodiscard]] std::expected<void, domain::DnsErrorInfo> check_min_header_size(std::span<const std::uint8_t> response) {
    if (response.size() >= dns::HEADER_SIZE)
        return {};
    return std::unexpected(domain::DnsErrorInfo{
        domain::DnsError::PARSE,
        fmt::format("DNS response too short: {} bytes (minimum {})", response.size(), dns::HEADER_SIZE)});
}

[[nodiscard]] std::expected<void, domain::DnsErrorInfo> check_qr_bit(std::span<const std::uint8_t> response) {
    if ((response[2] & 0x80) != 0)
        return {};
    return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response has QR=0 (not a response)"});
}

[[nodiscard]] std::expected<void, domain::DnsErrorInfo> check_txid(std::span<const std::uint8_t> request,
                                                                   std::span<const std::uint8_t> response) {
    if (response[0] == request[0] && response[1] == request[1])
        return {};
    return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response transaction ID mismatch"});
}

[[nodiscard]] std::expected<void, domain::DnsErrorInfo> check_qdcount(std::span<const std::uint8_t> response) {
    const auto qdcount = Utils::Bytes::try_read_u16_be(response, 4);
    if (!qdcount)
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response too short for QDCOUNT"});
    if (*qdcount == 1)
        return {};
    return std::unexpected(domain::DnsErrorInfo{domain::DnsError::PARSE,
                                                fmt::format("DNS response QDCOUNT is {} (expected 1)", *qdcount)});
}

[[nodiscard]] std::expected<void, domain::DnsErrorInfo> check_question_echo(std::span<const std::uint8_t> request,
                                                                            std::span<const std::uint8_t> response) {
    const auto req_qs_end = question_section_end(request);
    const auto rsp_qs_end = question_section_end(response);

    if (!req_qs_end || !rsp_qs_end) {
        return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response has malformed question section"});
    }

    const auto req_qs_len = *req_qs_end - dns::HEADER_SIZE;
    const auto rsp_qs_len = *rsp_qs_end - dns::HEADER_SIZE;

    if (req_qs_len == rsp_qs_len && std::ranges::equal(std::span(request).subspan(dns::HEADER_SIZE, req_qs_len),
                                                       std::span(response).subspan(dns::HEADER_SIZE, rsp_qs_len))) {
        return {};
    }

    return std::unexpected(
        domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS response question section does not match the query"});
}
}  // anonymous namespace

// ===========================================================================
//  Public API — orchestrator
// ===========================================================================

namespace dns::Validator {
std::expected<void, domain::DnsErrorInfo> validate_response(std::span<const std::uint8_t> request,
                                                            std::span<const std::uint8_t> response) {
    auto result = check_min_header_size(response);
    if (!result)
        return result;

    result = check_qr_bit(response);
    if (!result)
        return result;

    result = check_txid(request, response);
    if (!result)
        return result;

    result = check_qdcount(response);
    if (!result)
        return result;

    return check_question_echo(request, response);
}
}  // namespace dns::Validator
