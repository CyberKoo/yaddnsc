//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_TYPE_H
#define YADDNSC_TYPE_H

enum class ip_version_type {
    UNSPECIFIED, IPV4, IPV6
};

enum class dns_record_type {
    A, AAAA, TXT, SOA
};

enum class dns_lookup_error_type {
    NX_DOMAIN, RETRY, NODATA, PARSE, UNKNOWN
};

struct dns_server {
    std::string ip_address;
    unsigned short port;
};

#endif //YADDNSC_TYPE_H
