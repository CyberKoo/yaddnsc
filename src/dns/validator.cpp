//
// Created by Kotarou on 2026/7/7.
//
#include "dns/validator.h"

#include <cstdint>
#include <string>
#include <span>
#include <vector>

#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "exception/dns_lookup.h"

namespace {

    // ── Constants ──
    constexpr size_t DNS_HEADER_SIZE = 12;

    // ── Low-level DNS wire-format helpers ──

    /// Skip a DNS wire-format name (QNAME) starting at @p offset.
    /// Handles normal labels, compression pointers (0xC0), and the root label (0x00).
    /// @return  The offset past the end of the name, or 0 on error.
    [[nodiscard]] size_t skip_name(std::span<const std::uint8_t> msg, size_t offset) noexcept {
        while (offset < msg.size()) {
            const auto label_len = msg[offset];
            if (label_len == 0) {
                return offset + 1; // root label terminates the name
            }
            if ((label_len & 0xC0) == 0xC0) {
                return offset + 2; // compression pointer — skip 2 bytes
            }
            offset += 1 + label_len;
        }
        return 0; // malformed — ran off the end
    }

    /// Find the end of the first question section starting at byte 12.
    /// @return  The offset past QNAME + QTYPE + QCLASS, or 0 on error.
    [[nodiscard]] size_t question_section_end(std::span<const std::uint8_t> msg) noexcept {
        if (msg.size() < 12) return 0;
        if (DNS::Util::read_u16_be(msg, 4) == 0) return 0;
        const auto off = skip_name(msg, 12);
        if (off == 0 || off + 4 > msg.size()) return 0;
        return off + 4; // skip QTYPE + QCLASS
    }

    // ── Individual validation checks ──

    void check_min_header_size(std::span<const std::uint8_t> response) {
        if (response.size() >= 12) return;
        throw DnsLookupException(
            fmt::format("DNS response too short: {} bytes (minimum 12)", response.size()),
            DNS::Error::PARSE
        );
    }

    void check_qr_bit(std::span<const std::uint8_t> response) {
        if ((response[2] & 0x80) != 0) return;
        throw DnsLookupException(
            "DNS response has QR=0 (not a response)",
            DNS::Error::PARSE
        );
    }

    void check_txid(std::span<const std::uint8_t> request,
                    std::span<const std::uint8_t> response) {
        if (response[0] == request[0] && response[1] == request[1]) return;
        throw DnsLookupException(
            "DNS response transaction ID mismatch",
            DNS::Error::PARSE
        );
    }

    void check_qdcount(std::span<const std::uint8_t> response) {
        const auto qdcount = DNS::Util::read_u16_be(response, 4);
        if (qdcount == 1) return;
        throw DnsLookupException(
            fmt::format("DNS response QDCOUNT is {} (expected 1)", qdcount),
            DNS::Error::PARSE
        );
    }

    void check_question_echo(std::span<const std::uint8_t> request,
                             std::span<const std::uint8_t> response) {
        const auto req_qs_end = question_section_end(request);
        const auto rsp_qs_end = question_section_end(response);

        if (req_qs_end == 0 || rsp_qs_end == 0) {
            throw DnsLookupException(
                "DNS response has malformed question section",
                DNS::Error::PARSE
            );
        }

        const auto req_qs_len = req_qs_end - DNS_HEADER_SIZE;
        const auto rsp_qs_len = rsp_qs_end - DNS_HEADER_SIZE;

        if (req_qs_len == rsp_qs_len &&
            std::ranges::equal(
                std::span(request).subspan(DNS_HEADER_SIZE, req_qs_len),
                std::span(response).subspan(DNS_HEADER_SIZE, rsp_qs_len))) {
            return;
        }

        throw DnsLookupException(
            "DNS response question section does not match the query",
            DNS::Error::PARSE
        );
    }

} // anonymous namespace

// ===========================================================================
//  Public API — orchestrator
// ===========================================================================

namespace DNS::Validator {
    void validate_response(std::span<const std::uint8_t> request,
                           std::span<const std::uint8_t> response) {
        check_min_header_size(response);
        check_qr_bit(response);
        check_txid(request, response);
        check_qdcount(response);
        check_question_echo(request, response);
    }
} // namespace DNS
