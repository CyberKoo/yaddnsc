//
// Created by Kotarou on 2026/6/28.
//
#include "util.h"

#include <arpa/nameser.h>

std::string_view DNS::error_to_str(Error error) {
    switch (error) {
        case Error::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case Error::RETRY:
            return "retry (TRY_AGAIN)";
        case Error::NODATA:
            return "no data (NO_DATA)";
        case Error::PARSE:
            return "DNS record parse error (PARSE)";
        case Error::CONNECTION:
            return "connection error (CONNECTION)";
        default:
            return "unknown DNS error";
    }
}

AddressFamily DNS::type_to_family(Type type) {
    switch (type) {
        case Type::A:
            return AddressFamily::IPV4;
        case Type::AAAA:
            return AddressFamily::IPV6;
        default:
            return AddressFamily::UNSPECIFIED;
    }
}

int DNS::to_ns_type(Type type) noexcept {
    switch (type) {
        case Type::A: return ns_t_a;
        case Type::AAAA: return ns_t_aaaa;
        case Type::TXT: return ns_t_txt;
        case Type::SOA: return ns_t_soa;
        default: return ns_t_invalid;
    }
}
