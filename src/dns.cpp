//
// Created by Kotarou on 2022/4/5.
//
#include "dns.h"

#include <string_view>

#include "dns_resolver.h"
#include "dns_record_parser.h"

std::vector<std::string>
DNS::resolve(std::string_view host, dns_record_type type, const std::optional<dns_server> &server) {
    DnsResolver resolver(server);
    auto raw_response = resolver.query(host, type);

    DnsRecordParser parser(raw_response.data(), raw_response.size());
    std::vector<std::string> result;
    result.reserve(parser.record_count());

    for (size_t i = 0; i < parser.record_count(); ++i) {
        result.push_back(parser.parse_record(i));
    }

    return result;
}

std::string_view DNS::error_to_str(dns_lookup_error_type error) {
    switch (error) {
        case dns_lookup_error_type::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_lookup_error_type::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_lookup_error_type::NODATA:
            return "no data (NO_DATA)";
        case dns_lookup_error_type::PARSE:
            return "dns record parse error";
        default:
            return "unknown error";
    }
}
