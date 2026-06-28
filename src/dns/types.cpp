//
// Created by Kotarou on 2026/6/28.
//
#include "types.h"

#include <arpa/nameser.h>

std::string_view DNS::error_to_str(dns_error_type error) {
    switch (error) {
        case dns_error_type::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_error_type::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_error_type::NODATA:
            return "no data (NO_DATA)";
        case dns_error_type::PARSE:
            return "DNS record parse error (PARSE)";
        case dns_error_type::CONNECTION:
            return "connection error (CONNECTION)";
        default:
            return "unknown DNS error";
    }
}

address_family_type DNS::dns2ip(dns_type type) {
    switch (type) {
        case dns_type::A:
            return address_family_type::IPV4;
        case dns_type::AAAA:
            return address_family_type::IPV6;
        default:
            return address_family_type::UNSPECIFIED;
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
