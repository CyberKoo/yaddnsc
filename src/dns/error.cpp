//
// Created by Kotarou on 2026/7/4.
//
#include "dns_error.h"

// ===========================================================================
// error_to_str — convert DnsError enum to a human-readable string.
// ===========================================================================

std::string_view error_to_str(DnsError error) {
    switch (error) {
        case DnsError::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case DnsError::RETRY:
            return "retry (TRY_AGAIN)";
        case DnsError::NODATA:
            return "no data (NO_DATA)";
        case DnsError::PARSE:
            return "DNS record parse error (PARSE)";
        case DnsError::CONNECTION:
            return "connection error (CONNECTION)";
        case DnsError::CONFIG:
            return "configuration error (CONFIG)";
        case DnsError::CANCELLED:
            return "cancelled (CANCELLED)";
        case DnsError::SERVER_REFUSED:
            return "server refused query (REFUSED)";
        case DnsError::UNKNOWN:
            return "unknown DNS error";
        default:
            return "unknown DNS error";
    }
}
