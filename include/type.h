//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_TYPE_H
#define YADDNSC_TYPE_H

#include <string>
#include <cstdint>

enum class address_family_type {
    UNSPECIFIED, IPV4, IPV6
};

enum class dns_type {
    A, AAAA, TXT, SOA
};

// NOTE: NX_DOMAIN (with underscore) is intentional. The system header
// <arpa/nameser.h> defines NXDOMAIN as ns_r_nxdomain, which would
// cause a preprocessor conflict if we used the bare NXDOMAIN name.
enum class dns_error_type {
    NX_DOMAIN, RETRY, NODATA, PARSE, CONNECTION, UNKNOWN
};

struct dns_server_type {
    std::string address;
    uint16_t port{53};
};

#endif //YADDNSC_TYPE_H
