//
// Created by Kotarou on 2026/7/7.
//
// EXPERIMENTAL: self-contained DNS parser (no libresolv).
// Not enabled by default — use parser_system for production.
//

#ifndef YADDNSC_DNS_PARSER_NATIVE_H
#define YADDNSC_DNS_PARSER_NATIVE_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dns/types.h"

namespace DNS {
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
        /// @param host  Optional hostname for sanity checking (CNAME chain detection).
        /// @return      Structured result with RCODE and raw answer records.
        [[nodiscard]] static ParsedResponse parse_response(std::span<const std::uint8_t> data,
                                                           const std::string &host = {});

        /// Convenience: parse all answer records and return pre-formatted
        /// string values (IPs, hostnames, text, etc.).
        ///
        /// Preferred entry point for callers that only need the RCODE and
        /// formatted values.
        ///
        /// @param data  Span covering the raw packet bytes.
        /// @param host  Optional hostname for sanity checking (CNAME chain detection).
        /// @return      Structured result with RCODE and pre-formatted record values.
        [[nodiscard]] static FormattedResponse parse_strings(std::span<const std::uint8_t> data,
                                                             const std::string &host = {});

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
        std::span<const std::uint8_t> wire_;

        // ── Internal parsing (fully self-contained, no libresolv) ──
        [[nodiscard]] static ParsedMessage parse_message(std::span<const std::uint8_t> data,
                                                             bool copy_rdata = true);

        // ── Name decompression (RFC 1035 §4.1.4) ──
        // Returns the decompressed name and advances `offset` past the wire-format name.
        [[nodiscard]] static std::string decompress_name(std::span<const std::uint8_t> wire, size_t &offset);

        // ── RDATA formatting ──
        [[nodiscard]] static std::string rdata_to_string(const ResourceRecord &rr, std::span<const std::uint8_t> wire);

        [[nodiscard]] static std::string format_a(std::span<const std::uint8_t> rdata) noexcept;

        [[nodiscard]] static std::string format_aaaa(std::span<const std::uint8_t> rdata) noexcept;

        [[nodiscard]] static std::string format_txt(std::span<const std::uint8_t> rdata);

        [[nodiscard]] static std::string format_domain_name(std::span<const std::uint8_t> wire, size_t rdata_offset);

        [[nodiscard]] static std::string format_mx(std::span<const std::uint8_t> wire, size_t rdata_offset);

        [[nodiscard]] static std::string format_soa(std::span<const std::uint8_t> wire, size_t rdata_offset);

        [[nodiscard]] static std::string format_srv(std::span<const std::uint8_t> wire, size_t rdata_offset);

        [[nodiscard]] static std::string format_generic(std::span<const std::uint8_t> rdata);

        // ── EDNS0 parsing ──
        [[nodiscard]] static std::optional<EdnsInfo> parse_edns(const ResourceRecord &rr);

        ParsedMessage message_;
    };
} // namespace DNS

#endif  // YADDNSC_DNS_PARSER_NATIVE_H
