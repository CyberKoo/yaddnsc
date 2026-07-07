//
// Created by Kotarou on 2026/7/7.
//
#include "dns/parser/parser_native.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <array>
#include <limits>
#include <ranges>
#include <string>
#include <vector>
#include <system_error>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "fmt.hpp"
#include "dns_error.h"
#include "exception/dns_lookup.h"

// =============================================================================
// Internal constants
// =============================================================================

namespace {

constexpr size_t DNS_HEADER_SIZE = 12;
constexpr size_t NAME_MAX_BYTES = 255;       // RFC 1035 §2.3.4
constexpr uint8_t MAX_LABEL_LENGTH = 63;      // RFC 1035 §2.3.4
constexpr int MAX_POINTER_DEPTH = 7;          // Cycle / indirection limit
constexpr size_t QUESTION_FIXED_SIZE = 4;     // QTYPE(2) + QCLASS(2)
constexpr size_t RR_FIXED_SIZE = 10;          // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)

// EDNS0 constants
constexpr uint16_t OPT_RR_TYPE = 41;
constexpr size_t OPT_NAME_SIZE = 1;           // Root label (0x00)

/// Flag bits in the DNS header's second flags byte (byte 3).
/// QR=0x80 is in byte 2; AA, TC, RD are in byte 2; RA, Z, RCODE are in byte 3.
constexpr uint8_t FLAGS_QR     = 0x80;  // byte 2
constexpr uint8_t FLAGS_AA     = 0x04;  // byte 2
constexpr uint8_t FLAGS_TC     = 0x02;  // byte 2
constexpr uint8_t FLAGS_RD     = 0x01;  // byte 2
constexpr uint8_t FLAGS3_RA    = 0x80;  // byte 3
constexpr uint8_t FLAGS3_RCODE = 0x0F;  // byte 3, lower 4 bits

// EDNS0 TTL field masks
constexpr uint32_t EDNS_TTL_RCODE_MASK  = 0xFF000000U;
constexpr uint32_t EDNS_TTL_VERSION_MASK = 0x00FF0000U;
constexpr uint32_t EDNS_TTL_DO_MASK     = 0x00008000U;

}  // anonymous namespace

// =============================================================================
// Big-endian read helpers
// =============================================================================

inline uint16_t DNS::DnsParser::read_u16(const data_type *p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                  static_cast<uint16_t>(p[1]));
}

inline uint32_t DNS::DnsParser::read_u32(const data_type *p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// =============================================================================
// Name decompression (RFC 1035 §4.1.4)
// =============================================================================

std::string DNS::DnsParser::decompress_name(const data_type *wire, size_t wire_len,
                                             size_t &offset) {
    // Track visited offsets to detect pointer cycles.
    std::unordered_set<size_t> visited;
    std::string result;
    result.reserve(NAME_MAX_BYTES);

    size_t current = offset;
    int indirections = 0;
    bool jumped = false;

    while (true) {
        if (current >= wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: offset {} beyond wire length {}", current,
                            wire_len),
                Error::PARSE);
        }

        const auto label_len = wire[current];

        // Detect pointer (top two bits set: 0xC0).
        if ((label_len & 0xC0) == 0xC0) {
            if (current + 2 > wire_len) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: pointer at offset {} truncated", current),
                    Error::PARSE);
            }

            const auto ptr_offset =
                static_cast<size_t>(((static_cast<size_t>(label_len) & 0x3F) << 8) | wire[current + 1]);

            if (!visited.insert(ptr_offset).second) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: repeated pointer to offset {} (cycle)", ptr_offset),
                    Error::PARSE);
            }

            if (indirections++ >= MAX_POINTER_DEPTH) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: too many indirections ({})", MAX_POINTER_DEPTH),
                    Error::PARSE);
            }

            if (!jumped) {
                // First pointer: advance the caller's offset past the pointer.
                offset = current + 2;
                jumped = true;
            }
            current = ptr_offset;
            continue;
        }

        // Normal label.
        if (label_len == 0) {
            // Root label — end of name.
            if (!jumped) {
                offset = current + 1;
            }
            break;
        }

        if (label_len > MAX_LABEL_LENGTH) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: invalid label length {} at offset {}",
                            label_len, current),
                Error::PARSE);
        }

        if (!result.empty()) {
            result += '.';
        }

        current++;
        if (current + label_len > wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: label of length {} extends past wire end",
                            label_len),
                Error::PARSE);
        }

        result.append(reinterpret_cast<const char *>(wire + current), label_len);
        current += label_len;
    }

    return result;
}

// =============================================================================
// RDATA format helpers
// =============================================================================

std::string DNS::DnsParser::format_a(const data_type *rdata) noexcept {
    std::array<char, INET_ADDRSTRLEN> buf{};
    inet_ntop(AF_INET, rdata, buf.data(), buf.size());
    return buf.data();
}

std::string DNS::DnsParser::format_aaaa(const data_type *rdata) noexcept {
    std::array<char, INET6_ADDRSTRLEN> buf{};
    inet_ntop(AF_INET6, rdata, buf.data(), buf.size());
    return buf.data();
}

std::string DNS::DnsParser::format_txt(const data_type *rdata, size_t rdlen) {
    // TXT: one or more <character-string> segments.
    // Each segment: length byte (1 octet) followed by that many bytes.
    // Concatenate all segments.
    std::string result;
    size_t pos = 0;
    while (pos < rdlen) {
        auto seg_len = static_cast<size_t>(rdata[pos]);
        if (pos + 1 + seg_len > rdlen) {
            throw DnsLookupException(
                fmt::format("Invalid TXT record: segment length {} exceeds RDATA at offset {}",
                            seg_len, pos),
                Error::PARSE);
        }
        if (!result.empty()) {
            // Separate multiple character-strings with a space.
            result += ' ';
        }
        result.append(reinterpret_cast<const char *>(rdata + pos + 1), seg_len);
        pos += 1 + seg_len;
    }
    return result;
}

std::string DNS::DnsParser::format_domain_name(const data_type *wire_begin,
                                                 const data_type *wire_end,
                                                 size_t rdata_offset) {
    // Decompress a domain name from an arbitrary position in the wire buffer.
    size_t name_offset = rdata_offset;
    const auto wire_len = static_cast<size_t>(wire_end - wire_begin);
    return decompress_name(wire_begin, wire_len, name_offset);
}

std::string DNS::DnsParser::format_mx(const data_type *wire_begin,
                                        const data_type *wire_end,
                                        size_t rdata_offset) {
    // MX: preference (2 bytes) + domain-name (compressed).
    const auto pref = read_u16(wire_begin + rdata_offset);
    size_t name_offset = rdata_offset + 2;
    const auto wire_len = static_cast<size_t>(wire_end - wire_begin);
    auto name = decompress_name(wire_begin, wire_len, name_offset);
    return fmt::format("{} {}", pref, name);
}

std::string DNS::DnsParser::format_soa(const data_type *wire_begin,
                                         const data_type *wire_end,
                                         size_t rdata_offset,
                                         [[maybe_unused]] size_t rdlen) {
    // SOA: MNAME (domain-name) + RNAME (domain-name) + 5 × uint32.
    const auto wire_len = static_cast<size_t>(wire_end - wire_begin);

    // Parse MNAME.
    size_t offset = rdata_offset;
    auto mname = decompress_name(wire_begin, wire_len, offset);

    // Parse RNAME.
    auto rname = decompress_name(wire_begin, wire_len, offset);

    // 5 × 32-bit integers (big-endian).
    const auto serial  = read_u32(wire_begin + offset);
    offset += 4;
    const auto refresh = read_u32(wire_begin + offset);
    offset += 4;
    const auto retry   = read_u32(wire_begin + offset);
    offset += 4;
    const auto expire  = read_u32(wire_begin + offset);
    offset += 4;
    const auto minimum = read_u32(wire_begin + offset);

    return fmt::format("{} {} {} {} {} {} {}", mname, rname, serial, refresh, retry, expire, minimum);
}

std::string DNS::DnsParser::format_srv(const data_type *wire_begin,
                                         const data_type *wire_end,
                                         size_t rdata_offset) {
    // SRV: priority (2) + weight (2) + port (2) + target (domain-name).
    const auto priority = read_u16(wire_begin + rdata_offset);
    const auto weight   = read_u16(wire_begin + rdata_offset + 2);
    const auto port     = read_u16(wire_begin + rdata_offset + 4);

    const auto wire_len = static_cast<size_t>(wire_end - wire_begin);
    size_t offset = rdata_offset + 6;
    auto target = decompress_name(wire_begin, wire_len, offset);

    return fmt::format("{} {} {} {}", priority, weight, port, target);
}

std::string DNS::DnsParser::format_generic(const data_type *rdata, size_t rdlen) {
    // Hex dump for unknown record types.
    std::string result;
    result.reserve(rdlen * 2 + 2);
    result = "0x";
    static constexpr auto hex = "0123456789abcdef";
    for (size_t i = 0; i < rdlen; ++i) {
        result += hex[(rdata[i] >> 4) & 0x0F];
        result += hex[rdata[i] & 0x0F];
    }
    return result;
}

// =============================================================================
// EDNS0 parsing (RFC 6891)
// =============================================================================

std::optional<DNS::EdnsInfo>
DNS::DnsParser::parse_edns(const DnsResourceRecord &rr) {
    if (rr.type != OPT_RR_TYPE) {
        return std::nullopt;
    }

    EdnsInfo info{};

    // CLASS field = UDP payload size (2 bytes, big-endian).
    info.udp_payload_size = rr.qclass;

    // TTL field encodes extended RCODE (upper 8 bits), version (next 8 bits),
    // DNSSEC OK bit (bit 15), and reserved zeros (lower 15 bits).
    const auto ttl_val = rr.ttl;
    info.extended_rcode = static_cast<uint8_t>((ttl_val & EDNS_TTL_RCODE_MASK) >> 24);
    info.version        = static_cast<uint8_t>((ttl_val & EDNS_TTL_VERSION_MASK) >> 16);
    info.dnssec_ok      = (ttl_val & EDNS_TTL_DO_MASK) != 0;

    // Parse EDNS0 options (variable-length).
    size_t offset = 0;
    while (offset + 4 <= rr.rdata.size()) {
        EdnsOption opt;
        opt.code = read_u16(rr.rdata.data() + offset);
        offset += 2;

        const auto opt_len = static_cast<size_t>(read_u16(rr.rdata.data() + offset));
        offset += 2;

        if (offset + opt_len > rr.rdata.size()) {
            throw DnsLookupException(
                fmt::format("EDNS0 option truncated: code={}, declared length={}, remaining={}",
                            opt.code, opt_len, rr.rdata.size() - offset),
                Error::PARSE);
        }
        opt.data.assign(rr.rdata.data() + offset, rr.rdata.data() + offset + opt_len);
        offset += opt_len;

        info.options.push_back(std::move(opt));
    }

    return info;
}

// =============================================================================
// RDATA dispatch
// =============================================================================

std::string
DNS::DnsParser::rdata_to_string(const DnsResourceRecord &rr,
                                 const data_type *wire_begin,
                                 const data_type *wire_end) {
    const auto rdata_ptr = rr.rdata.data();
    const auto rdlen = rr.rdata.size();

    switch (rr.type) {
        case static_cast<uint16_t>(RecordType::A):
            if (rdlen != 4) {
                throw DnsLookupException(
                    fmt::format("Invalid A record RDATA length: {}", rdlen), Error::PARSE);
            }
            return format_a(rdata_ptr);

        case static_cast<uint16_t>(RecordType::AAAA):
            if (rdlen != 16) {
                throw DnsLookupException(
                    fmt::format("Invalid AAAA record RDATA length: {}", rdlen), Error::PARSE);
            }
            return format_aaaa(rdata_ptr);

        case static_cast<uint16_t>(RecordType::TXT):
            return format_txt(rdata_ptr, rdlen);

        case static_cast<uint16_t>(RecordType::CNAME):
        case static_cast<uint16_t>(RecordType::NS):
        case static_cast<uint16_t>(RecordType::PTR):
            return format_domain_name(wire_begin, wire_end, rr.rdata_offset);

        case static_cast<uint16_t>(RecordType::MX):
            return format_mx(wire_begin, wire_end, rr.rdata_offset);

        case static_cast<uint16_t>(RecordType::SOA):
            return format_soa(wire_begin, wire_end, rr.rdata_offset, rdlen);

        case static_cast<uint16_t>(RecordType::SRV):
            return format_srv(wire_begin, wire_end, rr.rdata_offset);

        default: {
            const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
            const auto type_str = type_name.empty() ? "?" : type_name.data();
            throw DnsLookupException(
                fmt::format("DNS parsing: record type {} ({}) is not supported yet", type_str,
                            rr.type),
                Error::PARSE);
        }
    }
}

// =============================================================================
// Full message parser
// =============================================================================

DNS::ParsedMessage
DNS::DnsParser::parse_message(const data_type *data, size_t size) {
    if (data == nullptr || size < DNS_HEADER_SIZE) {
        throw DnsLookupException(
            fmt::format("DNS response too short: {} bytes (minimum 12)", size), Error::PARSE);
    }

    ParsedMessage m{};

    // ── Parse header (12 bytes) ──
    m.id = read_u16(data);
    const auto flags2 = data[2];
    const auto flags3 = data[3];
    m.qr     = (flags2 & FLAGS_QR) != 0;
    m.opcode = static_cast<uint8_t>((data[2] >> 3) & 0x0F);
    m.aa     = (flags2 & FLAGS_AA) != 0;
    m.tc     = (flags2 & FLAGS_TC) != 0;
    m.rd     = (flags2 & FLAGS_RD) != 0;
    m.ra     = (flags3 & FLAGS3_RA) != 0;
    m.rcode  = (flags3 & FLAGS3_RCODE);

    m.qdcount = read_u16(data + 4);
    m.ancount = read_u16(data + 6);
    m.nscount = read_u16(data + 8);
    m.arcount = read_u16(data + 10);

    size_t offset = DNS_HEADER_SIZE;

    // ── Parse question section ──
    m.questions.reserve(m.qdcount);
    for (uint16_t i = 0; i < m.qdcount; ++i) {
        DnsQuestion q{};
        q.qname = decompress_name(data, size, offset);
        if (offset + QUESTION_FIXED_SIZE > size) {
            throw DnsLookupException(
                fmt::format("DNS question section truncated at offset {}", offset), Error::PARSE);
        }
        q.qtype  = read_u16(data + offset);
        q.qclass = read_u16(data + offset + 2);
        offset += QUESTION_FIXED_SIZE;
        m.questions.push_back(std::move(q));
    }

    // ── Parse resource records (answers, authority, additional) ──

    // Local lambda to parse a single RR from the wire.
    auto parse_rr = [&]() -> DnsResourceRecord {
        DnsResourceRecord rr{};
        rr.name = decompress_name(data, size, offset);
        if (offset + RR_FIXED_SIZE > size) {
            throw DnsLookupException(
                fmt::format("DNS RR header truncated at offset {}", offset), Error::PARSE);
        }
        rr.type   = read_u16(data + offset);
        rr.qclass = read_u16(data + offset + 2);
        rr.ttl    = read_u32(data + offset + 4);
        const auto rdlength = static_cast<size_t>(read_u16(data + offset + 8));
        offset += RR_FIXED_SIZE;
        if (offset + rdlength > size) {
            throw DnsLookupException(
                fmt::format("DNS RDATA truncated at offset {} (declared {})", offset, rdlength),
                Error::PARSE);
        }
        rr.rdata_offset = offset;
        rr.rdata.assign(data + offset, data + offset + rdlength);
        offset += rdlength;
        return rr;
    };

    m.answers.reserve(m.ancount);
    for (uint16_t i = 0; i < m.ancount; ++i) {
        m.answers.push_back(parse_rr());
    }

    m.authorities.reserve(m.nscount);
    for (uint16_t i = 0; i < m.nscount; ++i) {
        m.authorities.push_back(parse_rr());
    }

    m.additionals.reserve(m.arcount);
    for (uint16_t i = 0; i < m.arcount; ++i) {
        auto rr = parse_rr();
        // Check for EDNS0 OPT pseudo-record (RFC 6891 §6.1).
        // The NAME must be the root label (0x00), and CLASS must carry a
        // non-zero UDP payload size.
        if (rr.type == OPT_RR_TYPE && rr.name.size() == OPT_NAME_SIZE && rr.qclass > 0) {
            m.edns = parse_edns(rr);
        }
        m.additionals.push_back(std::move(rr));
    }

    // ── Combine RCODE with EDNS0 extended RCODE ──
    if (m.edns.has_value()) {
        m.rcode = static_cast<uint8_t>(m.rcode | (m.edns->extended_rcode << 4));
    }

    return m;
}

// =============================================================================
// DnsParser public API
// =============================================================================

DNS::DnsParser::DnsParser(const data_type *data, size_t size)
    : wire_begin_(data), wire_size_(size) {
    message_ = parse_message(data, size);
    SPDLOG_TRACE(R"(DNS message parser initialised (message size: {}, answer count: {}))", size,
                 message_.ancount);
}

size_t DNS::DnsParser::record_count() const noexcept {
    return static_cast<size_t>(message_.ancount);
}

std::string DNS::DnsParser::parse_record(size_t index) const {
    const auto &msg = message_;
    if (index >= static_cast<size_t>(msg.ancount)) {
        throw DnsLookupException(
            fmt::format("DNS parse_record: index {} out of bounds (answer count: {})", index,
                        msg.ancount),
            Error::PARSE);
    }

    const auto &rr = msg.answers[index];
    [[maybe_unused]] const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
    SPDLOG_TRACE(R"(Parsing DNS record #{}: type={} ({}), name="{}")", index,
                 type_name.empty() ? "?" : type_name.data(), rr.type, rr.name);

    const auto wire_end = wire_begin_ + wire_size_;
    return rdata_to_string(rr, wire_begin_, wire_end);
}

std::vector<std::string>
DNS::DnsParser::parse_all(const data_type *data, size_t size,
                                  [[maybe_unused]] const std::string &host) {
    DnsParser parser(data, size);
    std::vector<std::string> result;
    result.reserve(parser.record_count());

    for (size_t i = 0; i < parser.record_count(); ++i) {
        auto record = parser.parse_record(i);
        SPDLOG_TRACE(R"(DNS answer #{} for "{}": {})", i, host, record);
        result.push_back(std::move(record));
    }

    return result;
}
