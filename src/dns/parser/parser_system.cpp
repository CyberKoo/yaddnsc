//
// Created by Kotarou on 2026/6/17.
//
// ── System DNS parser (libresolv) [DEPRECATED] ──
//
// ╔════════════════════════════════════════════════════════════════════╗
// ║  DEPRECATED — will be removed before the 1.0.0 release.          ║
// ║                                                                  ║
// ║  This is the legacy system DNS parser based on libresolv.  It is ║
// ║  superseded by the native parser (parser_native, no libresolv    ║
// ║  dependency), which is now the default.                          ║
// ║                                                                  ║
// ║  Only compilation fixes will be applied here.  No new features,  ║
// ║  improvements, or refactoring.                                   ║
// ╚════════════════════════════════════════════════════════════════════╝
//
//
#pragma message("YADDNSC: system libresolv parser implementation is DEPRECATED and will be removed before 1.0.0")

#include "dns/parser/parser_system.h"

#include <array>
#include <system_error>
#include <vector>

#include "exception/dns_lookup.h"

#include "fmt.hpp"
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <magic_enum/magic_enum.hpp>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

DNS::RecordParser::RecordParser(const std::span<const std::uint8_t> data) {
    if (ns_initparse(data.data(), static_cast<int>(data.size()), &message_) != 0) {
        throw DnsLookupException("Failed to parse DNS response message", DnsError::PARSE);
    }

    // Extract RCODE from the raw wire header (byte 3, low 4 bits).
    // ns_msg doesn't expose rcode directly, but the raw header is always
    // accessible via ns_msg_base().
    if (data.size() >= HEADER_SIZE) {
        rcode_ = data[3] & 0x0F;
    }

    SPDLOG_TRACE(R"(DNS record parser initialised (message size: {}, answer count: {}, rcode: {}))", data.size(),
                 ns_msg_count(message_, ns_s_an), rcode_);
}

size_t DNS::RecordParser::record_count() const noexcept {
    return static_cast<size_t>(ns_msg_count(message_, ns_s_an));
}

std::string DNS::RecordParser::parse_record(size_t index) const {
    ns_rr dns_resource{};
    if (ns_parserr(&message_, ns_s_an, static_cast<int>(index), &dns_resource)) {
        throw DnsLookupException(
            fmt::format("An error occurred when parsing DNS resource at index {}, detail: {}", index,
                        std::error_code{errno, std::generic_category()}.message()),
            DnsError::PARSE
        );
    }

    auto rdlen = ns_rr_rdlen(dns_resource);
    auto rdata = ns_rr_rdata(dns_resource);
    auto rr_type = ns_rr_type(dns_resource);

    // Construct a span for the full message buffer (needed by name decompression).
    const auto msg_span = std::span<const std::uint8_t>{
        ns_msg_base(message_), static_cast<size_t>(ns_msg_end(message_) - ns_msg_base(message_))
    };

    std::string value;
    switch (rr_type) {
        case ns_t_a:
            value = parse_a_record({rdata, static_cast<size_t>(rdlen)});
            break;

        case ns_t_aaaa:
            value = parse_aaaa_record({rdata, static_cast<size_t>(rdlen)});
            break;

        case ns_t_txt:
            value = parse_txt_record({rdata, static_cast<size_t>(rdlen)});
            break;

        case ns_t_ns:
        case ns_t_soa:
        case ns_t_cname:
            value = parse_domain_name_record(msg_span, rdata);
            break;

        case ns_t_mx:
            value = parse_mx_record(msg_span, rdata);
            break;

        default: {
            const auto type_name = magic_enum::enum_name(rr_type);
            const auto &type_str = type_name.empty() ? "?" : type_name;
            throw DnsLookupException(
                fmt::format("DNS parsing: record type {} ({}) is not supported yet", type_str,
                            magic_enum::enum_name(rr_type))
            );
        }
    }

    {
        const auto type_name = magic_enum::enum_name(rr_type);
        [[maybe_unused]] const auto type_str = type_name.empty() ? "?" : type_name;
        SPDLOG_TRACE(R"(Parsed DNS record #{}: type={} ({}), value="{}")", index, type_str,
                     magic_enum::enum_integer(rr_type), value);
    }

    return value;
}

std::string DNS::RecordParser::parse_a_record(const std::span<const std::uint8_t> rdata) {
    std::array<char, INET_ADDRSTRLEN> address_buffer{};
    inet_ntop(AF_INET, rdata.data(), address_buffer.data(), address_buffer.size());
    return address_buffer.data();
}

std::string DNS::RecordParser::parse_aaaa_record(const std::span<const std::uint8_t> rdata) {
    std::array<char, INET6_ADDRSTRLEN> address_buffer{};
    inet_ntop(AF_INET6, rdata.data(), address_buffer.data(), address_buffer.size());
    return address_buffer.data();
}

std::string DNS::RecordParser::parse_txt_record(const std::span<const std::uint8_t> rdata) {
    // <character-string>: length (1 octet), string
    if (rdata.size() < 1) {
        throw DnsLookupException("Invalid TXT record (no data)");
    }

    auto length = rdata[0];
    if (rdata.size() < static_cast<std::size_t>(1) + length) {
        throw DnsLookupException(
            fmt::format("Invalid TXT record: declared length {} exceeds remaining data length {}", length,
                        rdata.size() - 1)
        );
    }

    return {reinterpret_cast<const char *>(rdata.data() + 1), length};
}

std::string DNS::RecordParser::parse_domain_name_record(const std::span<const std::uint8_t> msg,
                                                        const std::uint8_t *rdata) {
    // <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg.data(), msg.data() + msg.size(), rdata, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}

std::string DNS::RecordParser::parse_mx_record(const std::span<const std::uint8_t> msg, const std::uint8_t *rdata) {
    // MX: preference (2 octets), <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg.data(), msg.data() + msg.size(), rdata + 2, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}

DNS::ParsedResponse DNS::RecordParser::parse_response(const std::span<const std::uint8_t> data,
                                                      [[maybe_unused]] const std::string &host) {
    RecordParser parser(data);
    ParsedResponse response;
    response.rcode = static_cast<Rcode>(parser.rcode());

    if (response.rcode == Rcode::NOERROR) {
        const auto count = parser.record_count();
        response.answers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            ns_rr dns_resource{};
            if (ns_parserr(&parser.message_, ns_s_an, static_cast<int>(i), &dns_resource)) {
                throw DnsLookupException(
                    fmt::format("An error occurred when parsing DNS resource at index {}", i),
                    DnsError::PARSE
                );
            }

            ResourceRecord rr;
            rr.name = ns_rr_name(dns_resource);
            rr.type = static_cast<std::uint16_t>(ns_rr_type(dns_resource));
            rr.qclass = static_cast<std::uint16_t>(ns_rr_class(dns_resource));
            rr.ttl = ns_rr_ttl(dns_resource);
            const auto rdlen = ns_rr_rdlen(dns_resource);
            const auto *rdp = ns_rr_rdata(dns_resource);
            rr.rdata.assign(rdp, rdp + rdlen);
            rr.rdata_offset = 0; // not available with libresolv

            response.answers.push_back(std::move(rr));
        }
    }

    return response;
}

DNS::FormattedResponse DNS::RecordParser::parse_strings(const std::span<const std::uint8_t> data,
                                                        [[maybe_unused]] const std::string &host) {
    RecordParser parser(data);
    FormattedResponse response;
    response.rcode = static_cast<Rcode>(parser.rcode());

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
