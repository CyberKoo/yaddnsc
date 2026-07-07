//
// Created by Kotarou on 2026/7/7.
//

#ifndef YADDNSC_DNS_PARSER_NATIVE_H
#define YADDNSC_DNS_PARSER_NATIVE_H

#include "dns/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DNS {

// =============================================================================
// DnsParser — self-contained DNS wire-format parser (no libresolv)
// =============================================================================

/// Parser for raw DNS response packets (wire format, RFC 1035).
///
/// Parses a complete DNS message (header, question, answer, authority,
/// additional sections) from a raw wire-format buffer.  Supports A, AAAA,
/// TXT, MX, CNAME, NS, PTR, SOA, SRV records and EDNS0 OPT (RFC 6891).
///
/// Fully self-contained — no dependency on libresolv or <arpa/nameser.h>.
class DnsParser {
public:
    using data_type = std::uint8_t;

    /// Construct a parser from a raw DNS response buffer.
    /// @param data  Pointer to the raw packet bytes.
    /// @param size  Total size of the packet in bytes.
    /// @throws DnsLookupException on malformed packets.
    explicit DnsParser(const data_type *data, size_t size);

    /// Return the number of answer records in the parsed response.
    [[nodiscard]] size_t record_count() const noexcept;

    /// Parse a single answer record at the given index.
    /// @param index  Zero-based index into the answer section.
    /// @return       The record value as a string (IP, hostname, text, etc.).
    /// @throws DnsLookupException if `index` is out of bounds or RDATA is invalid.
    [[nodiscard]] std::string parse_record(size_t index) const;

    /// Convenience: parse all answer records and return as a vector.
    ///
    /// This is the preferred entry point for most callers.
    ///
    /// @param data  Pointer to the raw packet bytes.
    /// @param size  Total size of the packet in bytes.
    /// @param host  Optional hostname for sanity checking (CNAME chain detection).
    /// @return      List of parsed record values (IPs, hostnames, text, etc.).
    [[nodiscard]] static std::vector<std::string>
    parse_all(const data_type *data, size_t size, const std::string &host = {});

    // ── Full-message accessors (EDNS0-aware) ──

    /// Return the fully parsed DNS message.
    [[nodiscard]] const ParsedMessage &message() const noexcept {
        return message_;
    }

    /// Return the EDNS0 OPT record info, if present.
    [[nodiscard]] const std::optional<EdnsInfo> &edns() const noexcept {
        return message_.edns;
    }

private:
    // Wire buffer for name decompression during RDATA formatting.
    const data_type *wire_begin_{nullptr};
    size_t wire_size_{0};

    // ── Internal parsing (fully self-contained, no libresolv) ──
    static ParsedMessage parse_message(const data_type *data, size_t size);

    // ── Name decompression (RFC 1035 §4.1.4) ──
    // Returns the decompressed name and advances `offset` past the wire-format name.
    [[nodiscard]] static std::string decompress_name(const data_type *wire, size_t wire_len,
                                                      size_t &offset);

    // ── RDATA formatting ──
    [[nodiscard]] static std::string
    rdata_to_string(const DnsResourceRecord &rr,
                    const data_type *wire_begin,
                    const data_type *wire_end);

    [[nodiscard]] static std::string format_a(const data_type *rdata) noexcept;
    [[nodiscard]] static std::string format_aaaa(const data_type *rdata) noexcept;
    [[nodiscard]] static std::string format_txt(const data_type *rdata, size_t rdlen);
    [[nodiscard]] static std::string
    format_domain_name(const data_type *wire_begin, const data_type *wire_end,
                       size_t rdata_offset);
    [[nodiscard]] static std::string
    format_mx(const data_type *wire_begin, const data_type *wire_end,
              size_t rdata_offset);
    [[nodiscard]] static std::string
    format_soa(const data_type *wire_begin, const data_type *wire_end,
               size_t rdata_offset, size_t rdlen);
    [[nodiscard]] static std::string
    format_srv(const data_type *wire_begin, const data_type *wire_end,
               size_t rdata_offset);
    [[nodiscard]] static std::string
    format_generic(const data_type *rdata, size_t rdlen);

    // ── Big-endian helpers ──
    [[nodiscard]] static std::uint16_t read_u16(const data_type *p) noexcept;
    [[nodiscard]] static std::uint32_t read_u32(const data_type *p) noexcept;

    // ── EDNS0 parsing ──
    static std::optional<EdnsInfo> parse_edns(const DnsResourceRecord &rr);

    ParsedMessage message_;
};

}  // namespace DNS

#endif  // YADDNSC_DNS_PARSER_NATIVE_H
