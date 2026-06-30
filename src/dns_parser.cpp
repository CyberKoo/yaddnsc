//
// Created by Kotarou on 2026/6/29.
//

#include "dns_parser.h"

#include <cstring>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "exception/dns_lookup_exception.h"

int dns_type_to_ns_type(dns_record_type type) noexcept {
    switch (type) {
        case dns_record_type::A:
            return ns_t_a;
        case dns_record_type::AAAA:
            return ns_t_aaaa;
        case dns_record_type::TXT:
            return ns_t_txt;
        case dns_record_type::SOA:
            return ns_t_soa;
        default:
            return ns_t_invalid;
    }
}

std::vector<std::string> parse_dns_response(const uint8_t *data, size_t size) {
    ns_msg dns_message{};
    if (ns_initparse(data, static_cast<int>(size), &dns_message) < 0) {
        throw DnsLookupException(
            fmt::format("ns_initparse failed: {}", strerror(errno)),
            dns_lookup_error_type::PARSE);
    }

    auto answers = ns_msg_count(dns_message, ns_s_an);
    std::vector<std::string> resolve_result;

    for (auto i = 0; i < answers; ++i) {
        ns_rr dns_resource{};
        if (ns_parserr(&dns_message, ns_s_an, i, &dns_resource)) {
            throw DnsLookupException(
                fmt::format("An error occurred when parsing DNS resource, detail: {}", strerror(errno)),
                dns_lookup_error_type::PARSE);
        }

        const int rdlen = ns_rr_rdlen(dns_resource);

        switch (ns_rr_type(dns_resource)) {
            case ns_t_a: {
                char address_buffer[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, ns_rr_rdata(dns_resource), address_buffer, INET6_ADDRSTRLEN);
                resolve_result.emplace_back(address_buffer);
                break;
            }
            case ns_t_aaaa: {
                char address_buffer[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, ns_rr_rdata(dns_resource), address_buffer, INET6_ADDRSTRLEN);
                resolve_result.emplace_back(address_buffer);
                break;
            }
            case ns_t_txt: {
                if (rdlen < 1) {
                    throw DnsLookupException("Invalid TXT record (no data)");
                }
                auto length = *ns_rr_rdata(dns_resource);
                if (rdlen < 1 + length) {
                    throw DnsLookupException("Invalid TXT record");
                }
                resolve_result.emplace_back(
                    std::string(reinterpret_cast<const char *>(ns_rr_rdata(dns_resource) + 1), length));
                break;
            }
            case ns_t_ns:
            case ns_t_soa:
            case ns_t_cname: {
                char nsname[NS_MAXDNAME];
                if (ns_name_uncompress(ns_msg_base(dns_message), ns_msg_end(dns_message),
                                       ns_rr_rdata(dns_resource), nsname, NS_MAXDNAME) < 0) {
                    throw DnsLookupException("ns_name_uncompress failed");
                }
                resolve_result.emplace_back(nsname);
                break;
            }
            case ns_t_mx: {
                char nsname[NS_MAXDNAME];
                if (ns_name_uncompress(ns_msg_base(dns_message), ns_msg_end(dns_message),
                                       ns_rr_rdata(dns_resource) + 2, nsname, NS_MAXDNAME) < 0) {
                    throw DnsLookupException("ns_name_uncompress failed");
                }
                resolve_result.emplace_back(nsname);
                break;
            }
            default:
                throw DnsLookupException(
                    fmt::format("DNS parsing: type {} is not supported yet",
                                static_cast<int>(ns_rr_type(dns_resource))));
        }
    }

    return resolve_result;
}
