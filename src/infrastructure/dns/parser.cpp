// Self-contained DNS wire-format parser implementation.
// See parser.h for details.
//
#include "parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <magic_enum/magic_enum.hpp>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <yaddnsc/util/format.hpp>

#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/validator.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"
#include "support/util/bytes.hpp"
#include "yaddnsc/util/string_util.hpp"

// =============================================================================
// Internal constants
// =============================================================================

namespace {
constexpr size_t NAME_MAX_BYTES = 255;     // RFC 1035 §2.3.4
constexpr uint8_t MAX_LABEL_LENGTH = 63;   // RFC 1035 §2.3.4
constexpr int MAX_POINTER_DEPTH = 7;       // Cycle / indirection limit
constexpr size_t QUESTION_FIXED_SIZE = 4;  // QTYPE(2) + QCLASS(2)
constexpr size_t RR_FIXED_SIZE = 10;       // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)

/// Flag bits in the DNS header's second flags byte (byte 3).
/// QR=0x80 is in byte 2; AA, TC, RD are in byte 2; RA, Z, RCODE are in byte 3.
constexpr uint8_t FLAGS_QR = 0x80;      // byte 2
constexpr uint8_t FLAGS_AA = 0x04;      // byte 2
constexpr uint8_t FLAGS_TC = 0x02;      // byte 2
constexpr uint8_t FLAGS_RD = 0x01;      // byte 2
constexpr uint8_t FLAGS3_RA = 0x80;     // byte 3
constexpr uint8_t FLAGS3_RCODE = 0x0F;  // byte 3, lower 4 bits

// EDNS0 TTL field masks
constexpr uint32_t EDNS_TTL_RCODE_MASK = 0xFF000000U;
constexpr uint32_t EDNS_TTL_VERSION_MASK = 0x00FF0000U;
constexpr uint32_t EDNS_TTL_DO_MASK = 0x00008000U;

// Preserve label boundaries and binary octets in the dot-delimited presentation.
// In particular, a literal dot inside one label is not a label separator.
void append_name_octet(std::string& out, const std::uint8_t byte) {
    if (byte == '.' || byte == '\\' || byte < 32 || byte == 127) {
        out += '\\';
        out += static_cast<char>('0' + byte / 100);
        out += static_cast<char>('0' + (byte / 10) % 10);
        out += static_cast<char>('0' + byte % 10);
    } else {
        out += static_cast<char>(byte);
    }
}
}  // anonymous namespace

// =============================================================================
// Name decompression (RFC 1035 §4.1.4)
// =============================================================================

std::string dns::decompress_name(const std::span<const std::uint8_t> wire, size_t& offset) {
    // Track visited offsets to detect pointer cycles.
    // Fixed-size array on the stack: zero allocation, cache-friendly.
    // MAX_POINTER_DEPTH bounds the worst-case chain length, so
    // a simple linear scan is both simpler and faster than a hash set.
    const auto wire_len = wire.size();
    std::array<size_t, MAX_POINTER_DEPTH> visited{};
    size_t visited_count = 0;
    size_t expanded_size = 1;  // Root byte, plus each length-prefixed label.
    std::string result;
    result.reserve(NAME_MAX_BYTES);

    size_t current = offset;
    int indirections = 0;
    bool jumped = false;

    while (true) {
        if (current >= wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: offset {} beyond wire length {}", current, wire_len),
                domain::DnsError::PARSE);
        }

        const auto label_len = wire[current];

        // Detect pointer (top two bits set: 0xC0).
        if ((label_len & 0xC0) == 0xC0) {
            if (current + 2 > wire_len) {
                throw DnsLookupException(fmt::format("DNS name decompression: pointer at offset {} truncated", current),
                                         domain::DnsError::PARSE);
            }

            const auto ptr_offset =
                static_cast<size_t>(((static_cast<size_t>(label_len) & 0x3F) << 8) | wire[current + 1]);

            if (indirections++ >= MAX_POINTER_DEPTH) {
                throw DnsLookupException(
                    fmt::format("DNS name decompression: too many indirections ({})", MAX_POINTER_DEPTH),
                    domain::DnsError::PARSE);
            }

            // Linear scan of visited offsets (at most MAX_POINTER_DEPTH iterations).
            for (size_t i = 0; i < visited_count; ++i) {
                if (visited[i] == ptr_offset) {
                    throw DnsLookupException(
                        fmt::format("DNS name decompression: repeated pointer to offset {} (cycle)", ptr_offset),
                        domain::DnsError::PARSE);
                }
            }
            visited[visited_count++] = ptr_offset;

            if (!jumped) {
                // First pointer: advance the caller's offset past the pointer.
                offset = current + 2;
                jumped = true;
            }
            current = ptr_offset;
            continue;
        }

        // Normal label.
        if (label_len == 0) [[likely]] {
            // Root label — end of name.
            if (!jumped) {
                offset = current + 1;
            }
            break;
        }

        if (label_len > MAX_LABEL_LENGTH) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: invalid label length {} at offset {}", label_len, current),
                domain::DnsError::PARSE);
        }

        if (!result.empty()) {
            result += '.';
        }

        current++;
        if (current + label_len > wire_len) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: label of length {} extends past wire end", label_len),
                domain::DnsError::PARSE);
        }

        for (const auto byte : wire.subspan(current, label_len))
            append_name_octet(result, byte);
        current += label_len;
        expanded_size += 1 + label_len;

        // Count wire octets, not escaped presentation characters.
        if (expanded_size > NAME_MAX_BYTES) {
            throw DnsLookupException(
                fmt::format("DNS name decompression: expanded name exceeds {} bytes", NAME_MAX_BYTES),
                domain::DnsError::PARSE);
        }
    }

    return result;
}

std::string dns::present_name(const std::string_view name) {
    // A dot separates labels and is kept as written; every other octet goes
    // through the same presentation rules decompress_name() applies.
    std::string presented;
    presented.reserve(name.size());
    for (const char raw : name) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '.') {
            presented += '.';
        } else {
            append_name_octet(presented, byte);
        }
    }
    return presented;
}

// =============================================================================
// RDATA format helpers
// =============================================================================

std::string dns::RecordParser::format_inet(const std::span<const std::uint8_t> rdata, const int family,
                                           const std::size_t expected_size, const std::string_view label) {
    if (rdata.size() != expected_size) [[unlikely]] {
        throw DnsLookupException(
            fmt::format("Invalid {} record: RDATA is {} byte(s) (expected {})", label, rdata.size(), expected_size),
            domain::DnsError::PARSE);
    }
    std::array<char, INET6_ADDRSTRLEN> buf{};  // large enough for both families
    if (inet_ntop(family, rdata.data(), buf.data(), buf.size()) == nullptr) [[unlikely]] {
        throw DnsLookupException(fmt::format("Invalid {} record: address conversion failed", label),
                                 domain::DnsError::PARSE);
    }
    return buf.data();
}

std::string dns::RecordParser::format_txt(const std::span<const std::uint8_t> rdata) {
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
                domain::DnsError::PARSE);
        }
        if (!result.empty()) {
            // Separate multiple character-strings with a space.
            result += ' ';
        }
        result.append(reinterpret_cast<const char*>(rdata.data() + pos + 1), seg_len);
        pos += 1 + seg_len;
    }
    return result;
}

std::string dns::RecordParser::format_domain_name(const std::span<const std::uint8_t> wire, size_t rdata_offset) {
    // Decompress a domain name from an arbitrary position in the wire buffer.
    size_t name_offset = rdata_offset;
    return decompress_name(wire, name_offset);
}

std::string dns::RecordParser::format_mx(const std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen) {
    // MX: preference (2 bytes) + domain-name (compressed).
    const auto rdata = wire.subspan(rdata_offset, rdlen);
    const auto pref = Utils::Bytes::try_read_u16_be(rdata, 0);
    if (!pref) [[unlikely]] {
        throw DnsLookupException("Invalid MX record: preference missing", domain::DnsError::PARSE);
    }
    size_t name_offset = rdata_offset + 2;
    auto name = decompress_name(wire, name_offset);
    // The name's wire representation must fit inside the RDATA; only a
    // compression pointer may leave it (RFC 1035 §4.1.4).
    if (name_offset > rdata_offset + rdlen) [[unlikely]] {
        throw DnsLookupException("Invalid MX record: name extends past RDATA", domain::DnsError::PARSE);
    }
    return fmt::format("{} {}", *pref, name);
}

std::string dns::RecordParser::format_soa(const std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen) {
    // SOA: MNAME (domain-name) + RNAME (domain-name) + 5 × uint32.
    const auto rdata = wire.subspan(rdata_offset, rdlen);

    // Parse MNAME.
    size_t offset = rdata_offset;
    auto mname = decompress_name(wire, offset);

    // Parse RNAME.
    auto rname = decompress_name(wire, offset);

    // The names' wire representation must fit inside the RDATA; only a
    // compression pointer may leave it (RFC 1035 §4.1.4).
    const auto consumed = offset - rdata_offset;
    if (consumed > rdlen) [[unlikely]] {
        throw DnsLookupException("Invalid SOA record: names extend past RDATA", domain::DnsError::PARSE);
    }
    if (rdlen - consumed < 20) [[unlikely]] {
        throw DnsLookupException("Invalid SOA record: RDATA truncated", domain::DnsError::PARSE);
    }

    // 5 × 32-bit integers (big-endian).
    const auto serial = Utils::Bytes::try_read_u32_be(rdata, consumed);
    const auto refresh = Utils::Bytes::try_read_u32_be(rdata, consumed + 4);
    const auto retry = Utils::Bytes::try_read_u32_be(rdata, consumed + 8);
    const auto expire = Utils::Bytes::try_read_u32_be(rdata, consumed + 12);
    const auto minimum = Utils::Bytes::try_read_u32_be(rdata, consumed + 16);
    if (!serial || !refresh || !retry || !expire || !minimum) [[unlikely]] {
        throw DnsLookupException("Invalid SOA record: fixed fields missing", domain::DnsError::PARSE);
    }

    return fmt::format("{} {} {} {} {} {} {}", mname, rname, *serial, *refresh, *retry, *expire, *minimum);
}

std::string dns::RecordParser::format_srv(const std::span<const std::uint8_t> wire, size_t rdata_offset, size_t rdlen) {
    // SRV: priority (2) + weight (2) + port (2) + target (domain-name).
    const auto rdata = wire.subspan(rdata_offset, rdlen);
    const auto priority = Utils::Bytes::try_read_u16_be(rdata, 0);
    const auto weight = Utils::Bytes::try_read_u16_be(rdata, 2);
    const auto port = Utils::Bytes::try_read_u16_be(rdata, 4);
    if (!priority || !weight || !port) [[unlikely]] {
        throw DnsLookupException("Invalid SRV record: fixed fields missing", domain::DnsError::PARSE);
    }

    size_t offset = rdata_offset + 6;
    auto target = decompress_name(wire, offset);
    if (offset > rdata_offset + rdlen) [[unlikely]] {
        throw DnsLookupException("Invalid SRV record: target extends past RDATA", domain::DnsError::PARSE);
    }

    return fmt::format("{} {} {} {}", *priority, *weight, *port, target);
}

// =============================================================================
// EDNS0 parsing (RFC 6891)
// =============================================================================

std::optional<dns::EdnsInfo> dns::RecordParser::parse_edns(const ResourceRecord& rr,
                                                           const std::span<const std::uint8_t> wire) {
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

    // Both parser entry points validate the same wire bytes, even without an RDATA copy.
    const auto length = Utils::Bytes::read_u16_be(wire.subspan(rr.rdata_offset - 2));
    const auto rdata = wire.subspan(rr.rdata_offset, length);
    size_t offset = 0;
    while (offset < rdata.size()) {
        const auto opt_code = Utils::Bytes::try_read_u16_be(rdata, offset);
        const auto opt_len_raw = Utils::Bytes::try_read_u16_be(rdata, offset + 2);
        if (!opt_code || !opt_len_raw) [[unlikely]] {
            throw DnsLookupException(fmt::format("EDNS0 option header truncated at offset {} (remaining {} bytes)",
                                                 offset, rdata.size() - offset),
                                     domain::DnsError::PARSE);
        }
        offset += 4;

        const auto opt_len = static_cast<size_t>(*opt_len_raw);
        if (offset + opt_len > rdata.size()) {
            throw DnsLookupException(fmt::format("EDNS0 option truncated: code={}, declared length={}, remaining={}",
                                                 *opt_code, opt_len, rdata.size() - offset),
                                     domain::DnsError::PARSE);
        }

        EdnsOption opt;
        opt.code = *opt_code;
        opt.data.assign(rdata.data() + offset, rdata.data() + offset + opt_len);
        offset += opt_len;

        info.options.push_back(std::move(opt));
    }

    return info;
}

// =============================================================================
// RDATA dispatch
// =============================================================================

std::string dns::RecordParser::rdata_to_string(const ResourceRecord& rr, const std::span<const std::uint8_t> wire) {
    // Read RDATA length from the wire buffer (the 2-byte rdlength field
    // always precedes RDATA at rdata_offset - 2 in a valid DNS packet).
    // This works regardless of whether rr.rdata was populated, and avoids
    // depending on the copied vector size.
    const auto rdlen =
        static_cast<size_t>((static_cast<size_t>(wire[rr.rdata_offset - 2]) << 8) | wire[rr.rdata_offset - 1]);
    const auto rdata = wire.subspan(rr.rdata_offset, rdlen);

    switch (rr.type) {
        case static_cast<std::uint16_t>(RecordType::A):
            if (rdlen != 4) [[unlikely]] {
                throw DnsLookupException(fmt::format("Invalid A record RDATA length: {}", rdlen),
                                         domain::DnsError::PARSE);
            }
            return format_inet(rdata, AF_INET, 4, "A");

        case static_cast<std::uint16_t>(RecordType::AAAA):
            if (rdlen != 16) [[unlikely]] {
                throw DnsLookupException(fmt::format("Invalid AAAA record RDATA length: {}", rdlen),
                                         domain::DnsError::PARSE);
            }
            return format_inet(rdata, AF_INET6, 16, "AAAA");

        case static_cast<std::uint16_t>(RecordType::TXT):
            return format_txt(rdata);

        case static_cast<std::uint16_t>(RecordType::CNAME):
        case static_cast<std::uint16_t>(RecordType::NS):
        case static_cast<std::uint16_t>(RecordType::PTR):
            return format_domain_name(wire, rr.rdata_offset);

        case static_cast<std::uint16_t>(RecordType::MX):
            if (rdlen < 3) [[unlikely]] {
                throw DnsLookupException(fmt::format("Invalid MX record RDATA length: {}", rdlen),
                                         domain::DnsError::PARSE);
            }
            return format_mx(wire, rr.rdata_offset, rdlen);

        case static_cast<std::uint16_t>(RecordType::SOA):
            if (rdlen < 22) [[unlikely]] {
                throw DnsLookupException(fmt::format("Invalid SOA record RDATA length: {}", rdlen),
                                         domain::DnsError::PARSE);
            }
            return format_soa(wire, rr.rdata_offset, rdlen);

        case static_cast<std::uint16_t>(RecordType::SRV):
            if (rdlen < 7) [[unlikely]] {
                throw DnsLookupException(fmt::format("Invalid SRV record RDATA length: {}", rdlen),
                                         domain::DnsError::PARSE);
            }
            return format_srv(wire, rr.rdata_offset, rdlen);

        default: {
            const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
            const auto type_str = type_name.empty() ? "?" : type_name.data();
            throw DnsLookupException(fmt::format("DNS parsing: record type {} ({}) is not supported yet", type_str,
                                                 static_cast<std::uint16_t>(rr.type)),
                                     domain::DnsError::PARSE);
        }
    }
}

// =============================================================================
// Full message parser
// =============================================================================

dns::ResourceRecord dns::RecordParser::parse_rr(const std::span<const std::uint8_t> data, size_t& offset,
                                                const bool copy_rdata) {
    ResourceRecord rr{};
    rr.name = decompress_name(data, offset);
    if (offset + RR_FIXED_SIZE > data.size()) [[unlikely]] {
        throw DnsLookupException(fmt::format("DNS RR header truncated at offset {}", offset), domain::DnsError::PARSE);
    }
    rr.type = Utils::Bytes::read_u16_be(data.subspan(offset));
    rr.qclass = Utils::Bytes::read_u16_be(data.subspan(offset + 2));
    rr.ttl = Utils::Bytes::read_u32_be(data.subspan(offset + 4));
    const auto rd_length = static_cast<size_t>(Utils::Bytes::read_u16_be(data.subspan(offset + 8)));
    offset += RR_FIXED_SIZE;
    if (offset + rd_length > data.size()) [[unlikely]] {
        throw DnsLookupException(fmt::format("DNS RDATA truncated at offset {} (declared {})", offset, rd_length),
                                 domain::DnsError::PARSE);
    }
    rr.rdata_offset = offset;
    // Skip RDATA copy for the fast path (parse_strings) since
    // rdata_to_string now reads directly from the wire buffer.
    if (copy_rdata) [[likely]] {
        rr.rdata.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                        data.begin() + static_cast<std::ptrdiff_t>(offset + rd_length));
    }
    offset += rd_length;
    return rr;
}

dns::ParsedMessage dns::RecordParser::parse_message(const std::span<const std::uint8_t> data, const bool copy_rdata) {
    if (data.size() < HEADER_SIZE) [[unlikely]] {
        throw DnsLookupException(
            fmt::format("DNS packet too short: {} bytes (minimum {} bytes)", data.size(), HEADER_SIZE),
            domain::DnsError::PARSE);
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

    // Reject impossible counts before reserving attacker-controlled capacities.
    const auto minimum_size = HEADER_SIZE + size_t{m.qdcount} * QUESTION_FIXED_SIZE + size_t{m.qdcount} +
                              (size_t{m.ancount} + m.nscount + m.arcount) * (RR_FIXED_SIZE + 1);
    if (minimum_size > data.size()) {
        throw DnsLookupException("DNS section counts exceed the available packet bytes", domain::DnsError::PARSE);
    }
    size_t offset = HEADER_SIZE;

    // ── Parse question section ──
    m.questions.reserve(m.qdcount);
    for (uint16_t i = 0; i < m.qdcount; ++i) {
        Question q{};
        q.qname = decompress_name(data, offset);
        if (offset + QUESTION_FIXED_SIZE > data.size()) [[unlikely]] {
            throw DnsLookupException(fmt::format("DNS question section truncated at offset {}", offset),
                                     domain::DnsError::PARSE);
        }
        q.qtype = Utils::Bytes::read_u16_be(data.subspan(offset));
        q.qclass = Utils::Bytes::read_u16_be(data.subspan(offset + 2));
        offset += QUESTION_FIXED_SIZE;
        m.questions.push_back(std::move(q));
    }

    // ── Parse resource records (answers, authority, additional) ──

    m.answers.reserve(m.ancount);
    for (uint16_t i = 0; i < m.ancount; ++i) {
        m.answers.push_back(parse_rr(data, offset, copy_rdata));
    }

    m.authorities.reserve(m.nscount);
    for (uint16_t i = 0; i < m.nscount; ++i) {
        m.authorities.push_back(parse_rr(data, offset, copy_rdata));
    }

    m.additionals.reserve(m.arcount);
    for (uint16_t i = 0; i < m.arcount; ++i) {
        auto rr = parse_rr(data, offset, copy_rdata);
        // Check for EDNS0 OPT pseudo-record (RFC 6891 §6.1).
        // The NAME must be the root label (0x00, which decompresses to an
        // empty string), and CLASS must carry a non-zero UDP payload size.
        if (rr.type == static_cast<std::uint16_t>(RecordType::OPT)) [[unlikely]] {
            if (!rr.name.empty() || rr.qclass == 0) {
                throw DnsLookupException("Invalid OPT owner or payload size", domain::DnsError::PARSE);
            }
            if (m.edns) {
                throw DnsLookupException("DNS response has duplicate OPT records", domain::DnsError::PARSE);
            }
            m.edns = parse_edns(rr, data);
        }
        m.additionals.push_back(std::move(rr));
    }

    // ── Combine RCODE with EDNS0 extended RCODE ──
    if (m.edns.has_value()) [[unlikely]] {
        m.rcode = static_cast<Rcode>(static_cast<std::uint16_t>(m.rcode) | (m.edns->extended_rcode << 4));
    }

    return m;
}

// =============================================================================
// RecordParser public API
// =============================================================================

bool dns::name_matches(std::string_view first, std::string_view second) noexcept {
    if (first.ends_with('.'))
        first.remove_suffix(1);
    if (second.ends_with('.'))
        second.remove_suffix(1);
    return StringUtil::iequals(first, second);
}

dns::Question dns::RecordParser::parse_question(const std::span<const std::uint8_t> data) {
    size_t offset = HEADER_SIZE;
    Question question{};
    question.qname = decompress_name(data, offset);
    if (offset + QUESTION_FIXED_SIZE > data.size()) {
        throw DnsLookupException("DNS question section truncated", domain::DnsError::PARSE);
    }
    question.qtype = Utils::Bytes::read_u16_be(data.subspan(offset));
    question.qclass = Utils::Bytes::read_u16_be(data.subspan(offset + 2));
    return question;
}

dns::RecordParser::RecordParser(const std::span<const std::uint8_t> data) : wire_(data) {
    message_ = parse_message(data);
    SPDLOG_TRACE(R"(DNS message parser initialised (message size: {}, answer count: {}))", data.size(),
                 message_.ancount);
}

size_t dns::RecordParser::record_count() const noexcept {
    return static_cast<size_t>(message_.ancount);
}

std::string dns::RecordParser::parse_record(size_t index) const {
    const auto& msg = message_;
    if (index >= static_cast<size_t>(msg.ancount)) {
        throw DnsLookupException(
            fmt::format("DNS parse_record: index {} out of bounds (answer count: {})", index, msg.ancount),
            domain::DnsError::PARSE);
    }

    const auto& rr = msg.answers[index];
    [[maybe_unused]] const auto type_name = magic_enum::enum_name(static_cast<RecordType>(rr.type));
    SPDLOG_TRACE(R"(Parsing DNS record #{}: type={} ({}), name="{}")", index,
                 type_name.empty() ? "?" : type_name.data(), rr.type, rr.name);

    return rdata_to_string(rr, wire_);
}

dns::ParsedResponse dns::RecordParser::parse_response(const std::span<const std::uint8_t> data, const std::string& host,
                                                      const std::optional<RecordType> type_filter) {
    auto message = parse_message(data);
    ParsedResponse response;
    response.rcode = message.rcode;

    if (response.rcode == Rcode::NOERROR) {
        Validator::select_answers(message, data, host, type_filter);
        if (type_filter) {
            std::erase_if(message.answers, [type_filter](const auto& rr) {
                return rr.type != static_cast<std::uint16_t>(*type_filter);
            });
        }
        response.answers = std::move(message.answers);
    }

    return response;
}

dns::FormattedResponse dns::RecordParser::parse_strings(const std::span<const std::uint8_t> data,
                                                        const std::string& host,
                                                        const std::optional<RecordType> type_filter) {
    // Fast path: parse without copying RDATA — rdata_to_string reads
    // directly from the wire buffer via rdata_offset.
    auto msg = parse_message(data, false);
    FormattedResponse response;
    response.rcode = msg.rcode;

    if (response.rcode == Rcode::NOERROR) [[likely]] {
        Validator::select_answers(msg, data, host, type_filter);
        response.records.reserve(msg.answers.size());
        for (const auto& answer : msg.answers) {
            // Skip records that do not answer the query — above all the CNAME
            // hops of an RFC 1034 §3.6.2 chain, whose value is a hostname, not
            // the address the caller asked for.
            if (type_filter.has_value() && answer.type != static_cast<std::uint16_t>(*type_filter)) {
                continue;
            }
            auto record = rdata_to_string(answer, data);
            // Numbered by their position in the returned vector, so a trace
            // line maps onto response.records by index.
            SPDLOG_TRACE(R"(DNS answer #{} for "{}": {})", response.records.size(), host, record);
            response.records.push_back(std::move(record));
        }
    }

    return response;
}
