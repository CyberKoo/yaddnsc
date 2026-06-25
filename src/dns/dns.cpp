//
// Created by Kotarou on 2022/4/5.
//
#include "dns.h"

#include <string_view>

#include <spdlog/spdlog.h>

#include "dns_record_parser.h"
#include "dns_resolver.h"
#include "fmt.hpp"

std::vector<std::string>
DNS::resolve(const std::string &host, dns_type type, const std::vector<DnsServer> &servers) {
    const DnsResolver resolver(servers);
    auto raw_response = resolver.query(host, type);

    const DnsRecordParser parser(raw_response.data(), raw_response.size());
    std::vector<std::string> result;
    result.reserve(parser.record_count());

    for (size_t i = 0; i < parser.record_count(); ++i) {
        auto record = parser.parse_record(i);
        SPDLOG_TRACE(R"(DNS answer #{} for "{}": {})", i, host, record);
        result.push_back(std::move(record));
    }

    if (!result.empty()) {
        SPDLOG_DEBUG(R"(DNS lookup for "{}" returned {} record(s): {})",
                     host, result.size(), fmt::join(result, ", "));
    } else {
        SPDLOG_DEBUG(R"(DNS lookup for "{}" returned no records)", host);
    }

    return result;
}

address_family DNS::dns2ip(dns_type type) {
    switch (type) {
        case dns_type::A:
            return address_family::IPV4;
        case dns_type::AAAA:
            return address_family::IPV6;
        default:
            return address_family::UNSPECIFIED;
    }
}

std::string_view DNS::error_to_str(dns_error error) {
    switch (error) {
        case dns_error::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_error::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_error::NODATA:
            return "no data (NO_DATA)";
        case dns_error::PARSE:
            return "DNS record parse error (PARSE)";
        default:
            return "unknown DNS error";
    }
}
