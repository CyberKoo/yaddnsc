//
// Created by Kotarou on 2026/7/4.
//
#include "dns_error.h"

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
