// Self-contained DNS wire-format parser (RFC 1035).
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_PARSER_H
#define YADDNSC_INFRASTRUCTURE_DNS_PARSER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "infrastructure/dns/types.h"

namespace dns {
/// DNS name equality: ASCII case-insensitive, ignoring the terminal root dot.
[[nodiscard]] bool name_matches(std::string_view first, std::string_view second) noexcept;

/// Decode a wire-format name at `offset`, following compression pointers
/// (RFC 1035 §4.1.4), and advance `offset` past it. The result is presented in
/// dotted-decimal form, so label boundaries survive.
/// @throws DnsLookupException on malformed input.
[[nodiscard]] std::string decompress_name(std::span<const std::uint8_t> wire, std::size_t& offset);

/// Present a name in the same dotted-decimal form decompress_name() produces:
/// a literal dot, backslash or control octet is escaped. Dots stay separators.
[[nodiscard]] std::string present_name(std::string_view name);

// =============================================================================
// RecordParser — self-contained DNS wire-format parser (no libresolv)
// =============================================================================

/// Parser for raw DNS response packets (wire format, RFC 1035).
///
/// Parses a complete DNS message (header, question, answer, authority,
/// additional sections) from a raw wire-format buffer.  Supports A, AAAA,
/// TXT, MX, CNAME, NS, PTR, SOA, SRV records and EDNS0 OPT (RFC 6891).
///
/// Fully self-contained — no dependency on libresolv or <arpa/nameser.h>.
class RecordParser {
public:
    /// Construct a parser from a raw DNS response buffer.
    /// @param data  Span covering the raw packet bytes.
    /// @throws DnsLookupException on malformed packets.
    explicit RecordParser(std::span<const std::uint8_t> data);

    /// Decode the first question without requiring a complete RR section.
    /// Used to match even a truncated UDP reply. Throws DnsLookupException on malformed input.
    [[nodiscard]] static Question parse_question(std::span<const std::uint8_t> data);

    /// Return the number of answer records in the parsed response.
    [[nodiscard]] size_t record_count() const noexcept;

    /// Parse a single answer record at the given index.
    /// @param index  Zero-based index into the answer section.
    /// @return       The record value as a string (IP, hostname, text, etc.).
    /// @throws DnsLookupException if `index` is out of bounds or RDATA is invalid.
    [[nodiscard]] std::string parse_record(size_t index) const;

    /// Parse a raw DNS response and return the full ParsedResponse with
    /// original ResourceRecord answers (raw RDATA, TTL, type).
    ///
    /// @param data  Span covering the raw packet bytes.
    /// @param host  When nonempty, retain only IN answers for this name or its CNAME chain.
    ///              An empty host disables answer filtering altogether — the diagnostic
    ///              mode that hands back every answer of the packet. A caller issuing a
    ///              real query must normalise its name first (e.g. the empty string to
    ///              "."), otherwise foreign owners and foreign classes reach the caller.
    /// @return      Structured result with RCODE and raw answer records.
    /// @throws DnsLookupException on malformed packets.
    [[nodiscard]] static ParsedResponse parse_response(std::span<const std::uint8_t> data, const std::string& host = {},
                                                       std::optional<RecordType> type_filter = std::nullopt);

    /// Convenience: parse the answer records and return pre-formatted
    /// string values (IPs, hostnames, text, etc.).
    ///
    /// Preferred entry point for callers that only need the RCODE and
    /// formatted values.
    ///
    /// @param data         Span covering the raw packet bytes.
    /// @param host         Same host contract as parse_response: an empty host skips
    ///                     the owner/class filtering for diagnostic use, and a caller
    ///                     issuing a real query must normalise its name first
    ///                     (e.g. "" to "."). type_filter applies either way.
    /// @param type_filter  When set, only answer records of this type are
    ///                     formatted and returned: CNAME hops and any other
    ///                     non-matching records are skipped, so an RFC 1034
    ///                     §3.6.2 chain never surfaces a hostname where the
    ///                     caller expects an address.
    /// @return      Structured result with RCODE and pre-formatted record values.
    /// @throws DnsLookupException on malformed packets.
    [[nodiscard]] static FormattedResponse parse_strings(std::span<const std::uint8_t> data,
                                                         const std::string& host = {},
                                                         std::optional<RecordType> type_filter = std::nullopt);

    // ── Full-message accessors (EDNS0-aware) ──

    /// Return the fully parsed DNS message.
    [[nodiscard]] const ParsedMessage& message() const noexcept { return message_; }

    /// Return the EDNS0 OPT record info, if present.
    [[nodiscard]] const std::optional<EdnsInfo>& edns() const noexcept { return message_.edns; }

private:
    // Wire buffer for name decompression during RDATA formatting.
    std::span<const std::uint8_t> wire_;

    // ── Internal parsing (fully self-contained, no libresolv) ──
    [[nodiscard]] static ParsedMessage parse_message(std::span<const std::uint8_t> data, bool copy_rdata = true);

    // ── Resource-record parsing ──
    // Parses a single RR from the wire, advancing `offset` past it.
    [[nodiscard]] static ResourceRecord parse_rr(std::span<const std::uint8_t> data, size_t& offset, bool copy_rdata);

    // ── RDATA formatting ──
    [[nodiscard]] static std::string rdata_to_string(const ResourceRecord& rr, std::span<const std::uint8_t> wire);

    /// Shared A/AAAA formatter: validates the RDATA length and renders the
    /// address with inet_ntop. `family` is AF_INET/AF_INET6.
    [[nodiscard]] static std::string format_inet(std::span<const std::uint8_t> rdata, int family,
                                                 std::size_t expected_size, std::string_view label);

    [[nodiscard]] static std::string format_txt(std::span<const std::uint8_t> rdata);

    [[nodiscard]] static std::string format_domain_name(std::span<const std::uint8_t> wire, size_t rdata_offset);

    [[nodiscard]] static std::string format_mx(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    [[nodiscard]] static std::string format_soa(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    [[nodiscard]] static std::string format_srv(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    // ── EDNS0 parsing ──
    [[nodiscard]] static std::optional<EdnsInfo> parse_edns(const ResourceRecord& rr,
                                                            std::span<const std::uint8_t> wire);

    ParsedMessage message_;
};
}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_PARSER_H
