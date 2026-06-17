//
// Created by Kotarou on 2026/6/17.
//
#include "dns_record_parser.h"

#include <fmt/format.h>

#include <arpa/nameser.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <stdexcept>

#include "exception/dns_lookup_exception.h"

DnsRecordParser::DnsRecordParser(const unsigned char *data, size_t size) {
    if (ns_initparse(data, static_cast<int>(size), &message_) != 0) {
        throw DnsLookupException("Failed to parse DNS response message",
                                 dns_lookup_error_type::PARSE);
    }
}

size_t DnsRecordParser::record_count() const noexcept {
    return static_cast<size_t>(ns_msg_count(message_, ns_s_an));
}

std::string DnsRecordParser::parse_record(size_t index) const {
    ns_rr dns_resource{};
    if (ns_parserr(const_cast<ns_msg *>(&message_), ns_s_an, static_cast<int>(index), &dns_resource)) {
        throw DnsLookupException(
            fmt::format("An error occurred when parsing DNS resource at index {}, detail: {}",
                        index, strerror(errno)),
            dns_lookup_error_type::PARSE);
    }

    auto rdlen = ns_rr_rdlen(dns_resource);
    auto rdata = ns_rr_rdata(dns_resource);

    switch (ns_rr_type(dns_resource)) {
        case ns_t_a:
            return parse_a_record(rdata);

        case ns_t_aaaa:
            return parse_aaaa_record(rdata);

        case ns_t_txt:
            return parse_txt_record(rdata, rdlen);

        case ns_t_ns:
        case ns_t_soa:
        case ns_t_cname:
            return parse_domain_name_record(ns_msg_base(message_), ns_msg_end(message_), rdata);

        case ns_t_mx:
            return parse_mx_record(ns_msg_base(message_), ns_msg_end(message_), rdata);

        default:
            throw DnsLookupException(
                fmt::format("DNS parsing: record type {} is not supported yet",
                            static_cast<std::underlying_type_t<ns_type>>(ns_rr_type(dns_resource))));
    }
}

std::string DnsRecordParser::parse_a_record(const unsigned char *rdata) {
    char address_buffer[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, rdata, address_buffer, INET6_ADDRSTRLEN);
    return address_buffer;
}

std::string DnsRecordParser::parse_aaaa_record(const unsigned char *rdata) {
    char address_buffer[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET6, rdata, address_buffer, INET6_ADDRSTRLEN);
    return address_buffer;
}

std::string DnsRecordParser::parse_txt_record(const unsigned char *rdata, int rdlen) {
    // <character-string>: length (1 octet), string
    if (rdlen < 1) {
        throw DnsLookupException("Invalid TXT record (no data)");
    }

    auto length = *rdata;
    if (rdlen < 1 + length) {
        throw DnsLookupException("Invalid TXT record");
    }

    return {reinterpret_cast<const char *>(rdata + 1), length};
}

std::string DnsRecordParser::parse_domain_name_record(const unsigned char *msg_base,
                                                      const unsigned char *msg_end,
                                                      const unsigned char *rdata) {
    // <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg_base, msg_end, rdata, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}

std::string DnsRecordParser::parse_mx_record(const unsigned char *msg_base,
                                             const unsigned char *msg_end,
                                             const unsigned char *rdata) {
    // MX: preference (2 octets), <domain-name> (compressed)
    char nsname[NS_MAXDNAME];
    if (ns_name_uncompress(msg_base, msg_end, rdata + 2, nsname, NS_MAXDNAME) < 0) {
        throw DnsLookupException("ns_name_uncompress failed");
    }

    return nsname;
}
