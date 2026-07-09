//
// Created by Kotarou on 2026/7/7.
//

#ifndef YADDNSC_DNS_TYPES_H
#define YADDNSC_DNS_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace DNS {
    // =============================================================================
    // DNS wire-format type constants (RFC 1035, RFC 6891)
    // =============================================================================

    /// Size of the DNS header in wire format (RFC 1035 §4.1.1).
    constexpr size_t HEADER_SIZE = 12;

    /// DNS record types.
    enum class RecordType : std::uint16_t {
        A = 1,
        NS = 2,
        CNAME = 5,
        SOA = 6,
        MX = 15,
        TXT = 16,
        AAAA = 28,
        OPT = 41, // EDNS0 (RFC 6891)
        PTR = 12,
        SRV = 33,
    };

    /// DNS class codes.
    enum class RecordClass : std::uint16_t {
        IN = 1,
        ANY = 255,
    };

    /// DNS response codes (RCODE), including EDNS0 extended codes.
    enum class Rcode : std::uint8_t {
        NOERROR = 0,
        FORMERR = 1,
        SERVFAIL = 2,
        NXDOMAIN = 3,
        NOTIMP = 4,
        REFUSED = 5,
        YXDOMAIN = 6,
        YXRRSET = 7,
        NXRRSET = 8,
        NOTAUTH = 9,
        NOTZONE = 10,
        BADVERS = 16,
        BADCOOKIE = 23,
    };

    // =============================================================================
    // Parsed DNS structures
    // =============================================================================

    /// A single DNS question section entry.
    struct Question {
        std::string qname;
        std::uint16_t qtype;
        std::uint16_t qclass;
    };

    /// A single DNS resource record (answer, authority, or additional).
    struct ResourceRecord {
        std::string name;
        std::uint16_t type;
        std::uint16_t qclass;
        std::uint32_t ttl;
        std::vector<std::uint8_t> rdata;

        /// Offset of RDATA within the original wire buffer.
        /// Used for on-demand name decompression of domain-name RDATA (CNAME, NS, PTR, MX, SOA, SRV).
        size_t rdata_offset{0};
    };

    /// An EDNS0 option (code + data).
    struct EdnsOption {
        std::uint16_t code;
        std::vector<std::uint8_t> data;
    };

    /// Parsed EDNS0 OPT pseudo-record (RFC 6891).
    struct EdnsInfo {
        std::uint16_t udp_payload_size;
        std::uint8_t extended_rcode;
        std::uint8_t version;
        bool dnssec_ok;
        std::vector<EdnsOption> options;
    };

    /// Complete parsed DNS message, including EDNS0.
    struct ParsedMessage {
        // ── Header fields ──
        std::uint16_t id;
        bool qr;
        std::uint8_t opcode;
        bool aa;
        bool tc;
        bool rd;
        bool ra;

        std::uint16_t qdcount;
        std::uint16_t ancount;
        std::uint16_t nscount;
        std::uint16_t arcount;

        // ── Sections ──
        std::vector<Question> questions;
        std::vector<ResourceRecord> answers;
        std::vector<ResourceRecord> authorities;
        std::vector<ResourceRecord> additionals;

        // ── EDNS0 ──
        std::optional<EdnsInfo> edns;

        Rcode rcode{Rcode::NOERROR};
    };

    /// Structured result from RecordParser::parse_response, preserving RCODE
    /// and the original ResourceRecord answers.
    ///
    /// Dispatcher uses rcode for semantic decisions; callers that need the
    /// full wire-format RDATA (TTL, type, raw bytes) use this directly.
    struct ParsedResponse {
        Rcode rcode{Rcode::NOERROR};
        std::vector<ResourceRecord> answers;
    };

    /// Convenience result from RecordParser::parse_strings, with
    /// pre-formatted string values (IPs, hostnames, text, etc.).
    struct FormattedResponse {
        Rcode rcode{Rcode::NOERROR};
        std::vector<std::string> records;
    };
} // namespace DNS

#endif  // YADDNSC_DNS_TYPES_H
