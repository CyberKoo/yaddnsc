//
// Created by Kotarou on 2026/6/29.
//

#include "dns_parser.h"

#include <cstring>
#include <strings.h>
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

void validate_dns_response(const std::vector<uint8_t> &query, const std::vector<uint8_t> &response) {
    if (query.size() < 12 || query.size() > 65535 || response.size() < 12 || response.size() > 65535) {
        throw DnsLookupException("DNS packet has an invalid size", dns_lookup_error_type::PARSE);
    }
    if (query[0] != response[0] || query[1] != response[1]) {
        throw DnsLookupException("DNS response transaction ID does not match the query",
                                 dns_lookup_error_type::PARSE);
    }
    if ((response[2] & 0x80) == 0 || (query[2] & 0x78) != (response[2] & 0x78)) {
        throw DnsLookupException("DNS response flags do not match the query", dns_lookup_error_type::PARSE);
    }

    const auto question_count = [](const std::vector<uint8_t> &packet) {
        return static_cast<unsigned int>((static_cast<unsigned int>(packet[4]) << 8) | packet[5]);
    };
    if (question_count(query) != 1) {
        throw DnsLookupException("DNS query must contain one question", dns_lookup_error_type::PARSE);
    }
    if (question_count(response) == 0 && (response[3] & 0x0f) != 0) {
        return;
    }
    if (question_count(response) != 1) {
        throw DnsLookupException("DNS response must contain one question", dns_lookup_error_type::PARSE);
    }

    ns_msg request_message{};
    ns_msg response_message{};
    if (ns_initparse(query.data(), static_cast<int>(query.size()), &request_message) < 0 ||
        ns_initparse(response.data(), static_cast<int>(response.size()), &response_message) < 0) {
        throw DnsLookupException("Unable to parse DNS question for response validation",
                                 dns_lookup_error_type::PARSE);
    }
    ns_rr request_question{};
    ns_rr response_question{};
    if (ns_parserr(&request_message, ns_s_qd, 0, &request_question) != 0 ||
        ns_parserr(&response_message, ns_s_qd, 0, &response_question) != 0) {
        throw DnsLookupException("Unable to read DNS question for response validation",
                                 dns_lookup_error_type::PARSE);
    }
    if (strcasecmp(ns_rr_name(request_question), ns_rr_name(response_question)) != 0 ||
        ns_rr_type(request_question) != ns_rr_type(response_question) ||
        ns_rr_class(request_question) != ns_rr_class(response_question)) {
        throw DnsLookupException("DNS response question does not match the query", dns_lookup_error_type::PARSE);
    }
}

std::vector<std::string> parse_dns_response(const uint8_t *data, size_t size) {
    if (data == nullptr || size < 12 || size > 65535) {
        throw DnsLookupException("DNS response has an invalid size", dns_lookup_error_type::PARSE);
    }
    if ((data[2] & 0x80) == 0) {
        throw DnsLookupException("DNS packet is not a response", dns_lookup_error_type::PARSE);
    }
    if ((data[2] & 0x02) != 0) {
        throw DnsLookupException("DNS response is truncated", dns_lookup_error_type::PARSE);
    }

    const auto rcode = static_cast<int>(data[3] & 0x0f);
    switch (rcode) {
        case 0:
            break;
        case 2:
            throw DnsLookupException("DNS server returned SERVFAIL", dns_lookup_error_type::RETRY);
        case 3:
            throw DnsLookupException("DNS server returned NXDOMAIN", dns_lookup_error_type::NX_DOMAIN);
        case 5:
            throw DnsLookupException("DNS server refused the query", dns_lookup_error_type::UNKNOWN);
        default:
            throw DnsLookupException(fmt::format("DNS server returned RCODE {}", rcode),
                                     dns_lookup_error_type::UNKNOWN);
    }

    ns_msg dns_message{};
    if (ns_initparse(data, static_cast<int>(size), &dns_message) < 0) {
        throw DnsLookupException(
            fmt::format("ns_initparse failed: {}", strerror(errno)),
            dns_lookup_error_type::PARSE);
    }
    if (ns_msg_count(dns_message, ns_s_qd) != 1) {
        throw DnsLookupException("DNS response must contain one question", dns_lookup_error_type::PARSE);
    }
    ns_rr question{};
    if (ns_parserr(&dns_message, ns_s_qd, 0, &question) != 0) {
        throw DnsLookupException("Unable to parse DNS question", dns_lookup_error_type::PARSE);
    }
    const auto query_type = ns_rr_type(question);

    auto answers = ns_msg_count(dns_message, ns_s_an);
    std::vector<std::string> resolve_result;

    for (auto i = 0; i < answers; ++i) {
        ns_rr dns_resource{};
        if (ns_parserr(&dns_message, ns_s_an, i, &dns_resource)) {
            throw DnsLookupException(
                fmt::format("An error occurred when parsing DNS resource, detail: {}", strerror(errno)),
                dns_lookup_error_type::PARSE);
        }

        if (ns_rr_type(dns_resource) != query_type) {
            continue;
        }

        const int rdlen = ns_rr_rdlen(dns_resource);

        switch (ns_rr_type(dns_resource)) {
            case ns_t_a: {
                if (rdlen != NS_INADDRSZ) {
                    throw DnsLookupException(
                        fmt::format("Invalid A record: RDATA is {} byte(s) (expected {})", rdlen, NS_INADDRSZ),
                        dns_lookup_error_type::PARSE);
                }
                char address_buffer[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, ns_rr_rdata(dns_resource), address_buffer, INET6_ADDRSTRLEN);
                resolve_result.emplace_back(address_buffer);
                break;
            }
            case ns_t_aaaa: {
                if (rdlen != NS_IN6ADDRSZ) {
                    throw DnsLookupException(
                        fmt::format("Invalid AAAA record: RDATA is {} byte(s) (expected {})", rdlen, NS_IN6ADDRSZ),
                        dns_lookup_error_type::PARSE);
                }
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
