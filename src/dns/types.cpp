//
// Created by Kotarou on 2026/6/28.
//
#include "types.h"

#include <arpa/nameser.h>

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
        case dns_error::CONNECTION:
            return "connection error (CONNECTION)";
        default:
            return "unknown DNS error";
    }
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

int DNS::to_ns_type(dns_type type) noexcept {
    switch (type) {
        case dns_type::A: return ns_t_a;
        case dns_type::AAAA: return ns_t_aaaa;
        case dns_type::TXT: return ns_t_txt;
        case dns_type::SOA: return ns_t_soa;
        default: return ns_t_invalid;
    }
}
