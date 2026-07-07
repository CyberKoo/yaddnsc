//
// Created by Kotarou on 2026/6/17.
//
#include "dns/parser/parser_system.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/nameser.h>

#include <vector>
#include <system_error>
#include <array>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "fmt.hpp"
#include "exception/dns_lookup.h"

DNS::DnsParser::DnsParser(const data_type *data, const size_t size) {
    if (ns_initparse(data, static_cast<int>(size), &message_) != 0) {
        throw DnsLookupException("Failed to parse DNS response message", Error::PARSE);
    }

    SPDLOG_TRACE(R"(DNS record parser initialised (message size: {}, answer count: {}))", size,
                 ns_msg_count(message_, ns_s_an));
}

size_t DNS::DnsParser::record_count() const noexcept {
    return static_cast<size_t>(ns_msg_count(message_, ns_s_an));
}

std::string DNS::DnsParser::parse_record(size_t index) const {
    ns_rr dns_resource{};
    if (ns_parserr(&message_, ns_s_an, static_cast<int>(index), &dns_resource)) {
        throw DnsLookupException(
            fmt::format("An error occurred when parsing DNS resource at index {}, detail: {}", index, std::error_code{errno, std::generic_category()}.message()),
            Error::PARSE);
    }

    auto rdlen = ns_rr_rdlen(dns_resource);
    auto rdata = ns_rr_rdata(dns_resource);
    auto rr_type = ns_rr_type(dns_resource);

    std::string value;
    switch (rr_type) {
        case ns_t_a:
            value = parse_a_record(rdata);
            break;

        case ns_t_aaaa:
            value = parse_aaaa_record(rdata);
            break;

        case ns_t_txt:
            value = parse_txt_record(rdata, rdlen);
            break;

        case ns_t_ns:
        case ns_t_soa:
        case ns_t_cname:
            value = parse_domain_name_record(ns_msg_base(message_), ns_msg_end(message_), rdata);
            break;

        case ns_t_mx:
            value = parse_mx_record(ns_msg_base(message_), ns_msg_end(message_), rdata);
            break;

        default: {
            const auto type_name = magic_enum::enum_name(rr_type);
            const auto &type_str = type_name.empty() ? "?" : type_name;
            throw DnsLookupException(
                fmt::format("DNS parsing: record type {} ({}) is not supported yet", type_str,
                            magic_enum::enum_name(rr_type)
                )
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

std::string DNS::DnsParser::parse_a_record(const data_type *rdata) {
    std::array<char, INET_ADDRSTRLEN> address_buffer{};
    inet_ntop(AF_INET, rdata, address_buffer.data(), address_buffer.size());
    return address_buffer.data();
}

std::string DNS::DnsParser::parse_aaaa_record(const data_type *rdata) {
    std::array<char, INET6_ADDRSTRLEN> address_buffer{};
    inet_ntop(AF_INET6, rdata, address_buffer.data(), address_buffer.size());
    return address_buffer.data();
}

std::string DNS::DnsParser::parse_txt_record(const data_type *rdata, int rdlen) {
    // <character-string>: length (1 octet), string
    if (rdlen < 1) {
        throw DnsLookupException("Invalid TXT record (no data)");
    }

    auto length = *rdata;
    if (rdlen < 1 + length) {
        throw DnsLookupException(
            fmt::format("Invalid TXT record: declared length {} exceeds remaining data length {}", length, rdlen - 1)
        );
    }

    return {reinterpret_cast<const char *>(rdata + 1), length};
}

std::string DNS::DnsParser::parse_domain_name_record(const data_type *msg_base, const data_type *msg_end,
                                                           const data_type *rdata) {
    // <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg_base, msg_end, rdata, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}

std::string DNS::DnsParser::parse_mx_record(const data_type *msg_base, const data_type *msg_end,
                                                  const data_type *rdata) {
    // MX: preference (2 octets), <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg_base, msg_end, rdata + 2, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}

std::vector<std::string>
DNS::DnsParser::parse_all(const data_type *data, size_t size, [[maybe_unused]] const std::string &host) {
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
