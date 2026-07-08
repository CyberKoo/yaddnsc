//
// Created by Kotarou on 2026/7/4.
//

#ifndef YADDNSC_DNS_ERROR_H
#define YADDNSC_DNS_ERROR_H

#include <string_view>

/// DNS resolution error codes.
///
/// @note NX_DOMAIN (with underscore) is intentional. The system header
///       `<arpa/nameser.h>` defines `NXDOMAIN` as `ns_r_nxdomain`, which would
///       cause a preprocessor conflict if we used the bare NXDOMAIN name.
enum class DnsError {
    NX_DOMAIN,  ///< Domain does not exist (NXDOMAIN)
    RETRY,      ///< Transient failure, retry may succeed
    NODATA,     ///< Domain exists but no records of the requested type
    PARSE,      ///< Failed to parse DNS response
    CONNECTION, ///< Connection to DNS server failed
    CONFIG,     ///< Invalid configuration
    CANCELLED,  ///< Query was cancelled (another resolver answered first)
    SERVER_REFUSED,  ///< Server refused query (policy, rate-limit, not authoritative)
    UNKNOWN     ///< Unrecognised or uncategorised error
};

/// Convert a DnsError to a human-readable string.
/// @param error  The error code to stringify.
/// @return       A string view describing the error (e.g. "no such domain (NXDOMAIN)").
[[nodiscard]] std::string_view error_to_str(DnsError error);

#endif // YADDNSC_DNS_ERROR_H
