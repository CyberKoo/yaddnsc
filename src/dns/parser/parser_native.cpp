//
// Created by Kotarou on 2026/7/7.
//
// EXPERIMENTAL: Self-contained DNS wire-format parser implementation.
// See parser_native.h for details.
//
#include "dns/parser/parser_native.h"

#include <array>
#include <limits>
#include <ranges>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "exception/dns_lookup.h"
#include "util/bytes.hpp"

#include "dns_error.h"

#include "fmt.hpp"
#include <arpa/inet.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

// =============================================================================
// Internal constants
// =============================================================================

namespace {
    constexpr size_t NAME_MAX_BYTES = 255; // RFC 1035 §2.3.4
    constexpr uint8_t MAX_LABEL_LENGTH = 63; // RFC 1035 §2.3.4
    constexpr int MAX_POINTER_DEPTH = 7; // Cycle / indirection limit
    constexpr size_t QUESTION_FIXED_SIZE = 4; // QTYPE(2) + QCLASS(2)
    constexpr size_t RR_FIXED_SIZE = 10; // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)

    // EDNS0 constants
    constexpr size_t OPT_NAME_SIZE = 1; // Root label (0x00)

    /// Flag bits in the DNS header's second flags byte (byte 3).
    /// QR=0x80 is in byte 2; AA, TC, RD are in byte 2; RA, Z, RCODE are in byte 3.
    constexpr uint8_t FLAGS_QR = 0x80; // byte 2
    constexpr uint8_t FLAGS_AA = 0x04; // byte 2
    constexpr uint8_t FLAGS_TC = 0x02; // byte 2
    constexpr uint8_t FLAGS_RD = 0x01; // byte 2
    constexpr uint8_t FLAGS3_RA = 0x80; // byte 3
    constexpr uint8_t FLAGS3_RCODE = 0x0F; // byte 3, lower 4 bits

    // EDNS0 TTL field masks
    constexpr uint32_t EDNS_TTL_RCODE_MASK = 0xFF000000U;
    constexpr uint32_t EDNS_TTL_VERSION_MASK = 0x00FF0000U;
    constexpr uint32_t EDNS_TTL_DO_MASK = 0x00008000U;
} // anonymous namespace

// =============================================================================
// Name decompression (RFC 1035 §4.1.4)
// =============================================================================

std::string DNS::RecordParser::decompress_name(const std::span<const std::uint8_t> wire, size_t &offset) {
    // Track visited offsets to detect pointer cycles.
    const auto wire_len = wire.size();
    std::unordered_set<size_t> visited;
    std::string result;
    result.reserve(NAME_MAX_BYTES);

    size_t current = offset;
    int indirections = 0;
    bool jumped = false;

    while (true) {
        if (current >= wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: offset {} beyond wire length {}", current, wire_len),
                DnsError::PARSE);
        }

        const auto label_len = wire[current];

        // Detect pointer (top two bits set: 0xC0).
        if ((label_len & 0xC0) == 0xC0) {
            if (current + 2 > wire_len) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: pointer at offset {} truncated", current),
                    DnsError::PARSE
                );
            }

            const auto ptr_offset = static_cast<size_t>(
                ((static_cast<size_t>(label_len) & 0x3F) << 8) | wire[current + 1]);

            if (!visited.insert(ptr_offset).second) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: repeated pointer to offset {} (cycle)", ptr_offset),
                    DnsError::PARSE
                );
            }

            if (indirections++ >= MAX_POINTER_DEPTH) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: too many indirections ({})", MAX_POINTER_DEPTH),
                    DnsError::PARSE
                );
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
                fmt::format("DNS name decompression: invalid label length {} at offset {}", label_len, current),
                DnsError::PARSE
            );
        }

        if (!result.empty()) {
            result += '.';
        }

        current++;
        if (current + label_len > wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: label of length {} extends past wire end", label_len),
                DnsError::PARSE
            );
        }

        result.append(reinterpret_cast<const char *>(wire.data() + current), label_len);
        current += label_len;
    }

    return result;
}

// =============================================================================
// RDATA format helpers
// =============================================================================

std::string DNS::RecordParser::format_a(const std::span<const std::uint8_t> rdata) noexcept {
    std::array<char, INET_ADDRSTRLEN> buf{};
    inet_ntop(AF_INET, rdata.data(), buf.data(), buf.size());
    return buf.data();
}

std::string DNS::RecordParser::format_aaaa(const std::span<const std::uint8_t> rdata) noexcept {
    std::array<char, INET6_ADDRSTRLEN> buf{};
    inet_ntop(AF_INET6, rdata.data(), buf.data(), buf.size());
    return buf.data();
}

std::string DNS::RecordParser::format_txt(const std::span<const std::uint8_t> rdata) {
    // TXT: one or more <character-string> segments.
    // Each segment: length byte (1 octet) followed by that many bytes.
    // Concatenate all segments.
    const auto rdlen = rdata.size();
    std::string result;
    size_t pos = 0;
    while (pos < rdlen) {
        auto seg_len = static_cast<size_t>(rdata[pos]);
        if (pos + 1 + seg_len > rdlen) {
            throw DnsLookupException(
                fmt::format("Invalid TXT record: segment length {} exceeds RDATA at offset {}", seg_len, pos),
                DnsError::PARSE
            );
        }
        if (!result.empty()) {
            // Separate multiple character-strings with a space.
            result += ' ';
        }
        result.append(reinterpret_cast<const char *>(rdata.data() + pos + 1), seg_len);
        pos += 1 + seg_len;
    }
    return result;
}

std::string DNS::RecordParser::format_domain_name(const std::span<const std::uint8_t> wire, size_t rdata_offset) {
    // Decompress a domain name from an arbitrary position in the wire buffer.
    size_t name_offset = rdata_offset;
    return decompress_name(wire, name_offset);
}

std::string DNS::RecordParser::format_mx(const std::span<const std::uint8_t> wire, size_t rdata_offset) {
    // MX: preference (2 bytes) + domain-name (compressed).
    const auto pref = Utils::Bytes::read_u16_be(wire.subspan(rdata_offset));
    size_t name_offset = rdata_offset + 2;
    auto name = decompress_name(wire, name_offset);
    return fmt::format("{} {}", pref, name);
}

std::string DNS::RecordParser::format_soa(const std::span<const std::uint8_t> wire, size_t rdata_offset) {
    // SOA: MNAME (domain-name) + RNAME (domain-name) + 5 × uint32.

    // Parse MNAME.
    size_t offset = rdata_offset;
    auto mname = decompress_name(wire, offset);

    // Parse RNAME.
    auto rname = decompress_name(wire, offset);

    // 5 × 32-bit integers (big-endian).
    const auto serial = Utils::Bytes::read_u32_be(wire.subspan(offset));
    offset += 4;
    const auto refresh = Utils::Bytes::read_u32_be(wire.subspan(offset));
    offset += 4;
    const auto retry = Utils::Bytes::read_u32_be(wire.subspan(offset));
    offset += 4;
    const auto expire = Utils::Bytes::read_u32_be(wire.subspan(offset));
    offset += 4;
    const auto minimum = Utils::Bytes::read_u32_be(wire.subspan(offset));

    return fmt::format("{} {} {} {} {} {} {}", mname, rname, serial, refresh, retry, expire, minimum);
}

std::string DNS::RecordParser::format_srv(const std::span<const std::uint8_t> wire, size_t rdata_offset) {
    // SRV: priority (2) + weight (2) + port (2) + target (domain-name).
    const auto priority = Utils::Bytes::read_u16_be(wire.subspan(rdata_offset));
    const auto weight = Utils::Bytes::read_u16_be(wire.subspan(rdata_offset + 2));
    const auto port = Utils::Bytes::read_u16_be(wire.subspan(rdata_offset + 4));

    size_t offset = rdata_offset + 6;
    auto target = decompress_name(wire, offset);

    return fmt::format("{} {} {} {}", priority, weight, port, target);
}

std::string DNS::RecordParser::format_generic(const std::span<const std::uint8_t> rdata) {
    // Hex dump for unknown record types.
    const auto rdlen = rdata.size();
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

std::optional<DNS::EdnsInfo> DNS::RecordParser::parse_edns(const ResourceRecord &rr) {
    if (rr.type != static_cast<std::uint16_t>(RecordType::OPT)) {
        return std::nullopt;
    }

    EdnsInfo info{};

    // CLASS field = UDP payload size (2 bytes, big-endian).
    info.udp_payload_size = rr.qclass;

    // TTL field encodes extended RCODE (upper 8 bits), version (next 8 bits),
    // DNSSEC OK bit (bit 15), and reserved zeros (lower 15 bits).
    const auto ttl_val = rr.ttl;
    info.extended_rcode = static_cast<uint8_t>((ttl_val & EDNS_TTL_RCODE_MASK) >> 24);
    info.version = static_cast<uint8_t>((ttl_val & EDNS_TTL_VERSION_MASK) >> 16);
    info.dnssec_ok = (ttl_val & EDNS_TTL_DO_MASK) != 0;

    // Parse EDNS0 options (variable-length).
    size_t offset = 0;
    while (offset + 4 <= rr.rdata.size()) {
        EdnsOption opt;
        opt.code = Utils::Bytes::read_u16_be(rr.rdata, offset);
        offset += 2;

        const auto opt_len = static_cast<size_t>(Utils::Bytes::read_u16_be(rr.rdata, offset));
        offset += 2;

        if (offset + opt_len > rr.rdata.size()) {
            throw DnsLookupException(
                fmt::format("EDNS0 option truncated: code={}, declared length={}, remaining={}",
                            opt.code, opt_len, rr.rdata.size() - offset),
                DnsError::PARSE
            );
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

std::string DNS::RecordParser::rdata_to_string(const ResourceRecord &rr, const std::span<const std::uint8_t> wire) {
    const auto rdata = std::span{rr.rdata};
    const auto rdlen = rdata.size();

    switch (rr.type) {
        case static_cast<std::uint16_t>(RecordType::A):
            if (rdlen != 4) {
                throw DnsLookupException(
                    fmt::format("Invalid A record RDATA length: {}", rdlen),
                    DnsError::PARSE
                );
            }
            return format_a(rdata);

        case static_cast<std::uint16_t>(RecordType::AAAA):
            if (rdlen != 16) {
                throw DnsLookupException(
                    fmt::format("Invalid AAAA record RDATA length: {}", rdlen),
                    DnsError::PARSE
                );
            }
            return format_aaaa(rdata);

        case static_cast<std::uint16_t>(RecordType::TXT):
            return format_txt(rdata);

        case static_cast<std::uint16_t>(RecordType::CNAME):
        case static_cast<std::uint16_t>(RecordType::NS):
        case static_cast<std::uint16_t>(RecordType::PTR):
            return format_domain_name(wire, rr.rdata_offset);

        case static_cast<std::uint16_t>(RecordType::MX):
            return format_mx(wire, rr.rdata_offset);

        case static_cast<std::uint16_t>(RecordType::SOA):
            return format_soa(wire, rr.rdata_offset);

        case static_cast<std::uint16_t>(RecordType::SRV):
            return format_srv(wire, rr.rdata_offset);

        default: {
            const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
            const auto type_str = type_name.empty() ? "?" : type_name.data();
            throw DnsLookupException(
                fmt::format("DNS parsing: record type {} ({}) is not supported yet", type_str,
                            static_cast<std::uint16_t>(rr.type)),
                DnsError::PARSE
            );
        }
    }
}

// =============================================================================
// Full message parser
// =============================================================================

DNS::ParsedMessage DNS::RecordParser::parse_message(const std::span<const std::uint8_t> data) {
    if (data.size() < HEADER_SIZE) {
        throw DnsLookupException(
            fmt::format("DNS packet too short: {} bytes (minimum {} bytes)", data.size(), HEADER_SIZE),
            DnsError::PARSE
        );
    }

    ParsedMessage m{};

    // ── Parse header (12 bytes) ──
    m.id = Utils::Bytes::read_u16_be(data);
    const auto flags2 = data[2];
    const auto flags3 = data[3];
    m.qr = (flags2 & FLAGS_QR) != 0;
    m.opcode = static_cast<uint8_t>((data[2] >> 3) & 0x0F);
    m.aa = (flags2 & FLAGS_AA) != 0;
    m.tc = (flags2 & FLAGS_TC) != 0;
    m.rd = (flags2 & FLAGS_RD) != 0;
    m.ra = (flags3 & FLAGS3_RA) != 0;
    m.rcode = static_cast<Rcode>(flags3 & FLAGS3_RCODE);

    m.qdcount = Utils::Bytes::read_u16_be(data.subspan(4));
    m.ancount = Utils::Bytes::read_u16_be(data.subspan(6));
    m.nscount = Utils::Bytes::read_u16_be(data.subspan(8));
    m.arcount = Utils::Bytes::read_u16_be(data.subspan(10));

    size_t offset = HEADER_SIZE;

    // ── Parse question section ──
    m.questions.reserve(m.qdcount);
    for (uint16_t i = 0; i < m.qdcount; ++i) {
        Question q{};
        q.qname = decompress_name(data, offset);
        if (offset + QUESTION_FIXED_SIZE > data.size()) {
            throw DnsLookupException(
                fmt::format("DNS question section truncated at offset {}", offset),
                DnsError::PARSE
            );
        }
        q.qtype = Utils::Bytes::read_u16_be(data.subspan(offset));
        q.qclass = Utils::Bytes::read_u16_be(data.subspan(offset + 2));
        offset += QUESTION_FIXED_SIZE;
        m.questions.push_back(std::move(q));
    }

    // ── Parse resource records (answers, authority, additional) ──

    // Local lambda to parse a single RR from the wire.
    auto parse_rr = [&]() -> ResourceRecord {
        ResourceRecord rr{};
        rr.name = decompress_name(data, offset);
        if (offset + RR_FIXED_SIZE > data.size()) {
            throw DnsLookupException(
                fmt::format("DNS RR header truncated at offset {}", offset),
                DnsError::PARSE
            );
        }
        rr.type = Utils::Bytes::read_u16_be(data.subspan(offset));
        rr.qclass = Utils::Bytes::read_u16_be(data.subspan(offset + 2));
        rr.ttl = Utils::Bytes::read_u32_be(data.subspan(offset + 4));
        const auto rd_length = static_cast<size_t>(Utils::Bytes::read_u16_be(data.subspan(offset + 8)));
        offset += RR_FIXED_SIZE;
        if (offset + rd_length > data.size()) {
            throw DnsLookupException(
                fmt::format("DNS RDATA truncated at offset {} (declared {})", offset, rd_length),
                DnsError::PARSE
            );
        }
        rr.rdata_offset = offset;
        rr.rdata.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                        data.begin() + static_cast<std::ptrdiff_t>(offset + rd_length));
        offset += rd_length;
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
        if (rr.type == static_cast<std::uint16_t>(RecordType::OPT) && rr.name.size() == OPT_NAME_SIZE && rr.qclass >
            0) {
            m.edns = parse_edns(rr);
        }
        m.additionals.push_back(std::move(rr));
    }

    // ── Combine RCODE with EDNS0 extended RCODE ──
    if (m.edns.has_value()) {
        m.rcode = static_cast<Rcode>(static_cast<uint8_t>(m.rcode) | (m.edns->extended_rcode << 4));
    }

    return m;
}

// =============================================================================
// RecordParser public API
// =============================================================================

DNS::RecordParser::RecordParser(const std::span<const std::uint8_t> data) : wire_(data) {
    message_ = parse_message(data);
    SPDLOG_TRACE(R"(DNS message parser initialised (message size: {}, answer count: {}))", data.size(),
                 message_.ancount);
}

size_t DNS::RecordParser::record_count() const noexcept {
    return static_cast<size_t>(message_.ancount);
}

std::string DNS::RecordParser::parse_record(size_t index) const {
    const auto &msg = message_;
    if (index >= static_cast<size_t>(msg.ancount)) {
        throw DnsLookupException(
            fmt::format("DNS parse_record: index {} out of bounds (answer count: {})", index, msg.ancount),
            DnsError::PARSE
        );
    }

    const auto &rr = msg.answers[index];
    [[maybe_unused]] const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
    SPDLOG_TRACE(R"(Parsing DNS record #{}: type={} ({}), name="{}")", index,
                 type_name.empty() ? "?" : type_name.data(),
                 magic_enum::enum_integer(rr.type), rr.name);

    return rdata_to_string(rr, wire_);
}

DNS::ParsedResponse DNS::RecordParser::parse_response(const std::span<const std::uint8_t> data,
                                                      [[maybe_unused]] const std::string &host) {
    RecordParser parser(data);
    ParsedResponse response;
    response.rcode = parser.message().rcode;

    if (response.rcode == Rcode::NOERROR) {
        response.answers = parser.message().answers;
    }

    return response;
}

DNS::FormattedResponse DNS::RecordParser::parse_strings(const std::span<const std::uint8_t> data,
                                                        [[maybe_unused]] const std::string &host) {
    RecordParser parser(data);
    FormattedResponse response;
    response.rcode = parser.message().rcode;

    if (response.rcode == Rcode::NOERROR) {
        response.records.reserve(parser.record_count());
        for (size_t i = 0; i < parser.record_count(); ++i) {
            auto record = parser.parse_record(i);
            SPDLOG_TRACE(R"(DNS answer #{} for "{}": {})", i, host, record);
            response.records.push_back(std::move(record));
        }
    }

    return response;
}
