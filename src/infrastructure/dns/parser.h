// Self-contained DNS wire-format parser (RFC 1035).
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_PARSER_H
#define YADDNSC_INFRASTRUCTURE_DNS_PARSER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "infrastructure/dns/types.h"

namespace dns {
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
    /// @param host  Optional hostname, reserved for diagnostics.
    /// @return      Structured result with RCODE and raw answer records.
    /// @throws DnsLookupException on malformed packets.
    [[nodiscard]] static ParsedResponse parse_response(std::span<const std::uint8_t> data,
                                                       const std::string& host = {});

    /// Convenience: parse the answer records and return pre-formatted
    /// string values (IPs, hostnames, text, etc.).
    ///
    /// Preferred entry point for callers that only need the RCODE and
    /// formatted values.
    ///
    /// @param data         Span covering the raw packet bytes.
    /// @param host         Optional hostname, used in diagnostic logging.
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

    // ── Name decompression (RFC 1035 §4.1.4) ──
    // Returns the decompressed name and advances `offset` past the wire-format name.
    [[nodiscard]] static std::string decompress_name(std::span<const std::uint8_t> wire, size_t& offset);

    // ── Resource-record parsing ──
    // Parses a single RR from the wire, advancing `offset` past it.
    [[nodiscard]] static ResourceRecord parse_rr(std::span<const std::uint8_t> data, size_t& offset, bool copy_rdata);

    // ── RDATA formatting ──
    [[nodiscard]] static std::string rdata_to_string(const ResourceRecord& rr, std::span<const std::uint8_t> wire);

    [[nodiscard]] static std::string format_a(std::span<const std::uint8_t> rdata);

    [[nodiscard]] static std::string format_aaaa(std::span<const std::uint8_t> rdata);

    [[nodiscard]] static std::string format_txt(std::span<const std::uint8_t> rdata);

    [[nodiscard]] static std::string format_domain_name(std::span<const std::uint8_t> wire, size_t rdata_offset);

    [[nodiscard]] static std::string format_mx(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    [[nodiscard]] static std::string format_soa(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    [[nodiscard]] static std::string format_srv(std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen);

    // ── EDNS0 parsing ──
    [[nodiscard]] static std::optional<EdnsInfo> parse_edns(const ResourceRecord& rr);

    ParsedMessage message_;
};
}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_PARSER_H
