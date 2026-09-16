//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_ERROR_H
#define YADDNSC_DOMAIN_ERROR_H

#include <string>

/// Domain-level error types.
///
/// Shared convention: every error carries a category-specific `code` plus a
/// human-readable `message`. Recoverable errors cross layer boundaries as
/// values inside std::expected, never as exceptions. Only the types with a
/// real caller are fleshed out; the rest are skeletons filled in by later
/// phases (see refactor/01-architecture-and-contracts.md §3).
///
/// DNS already conforms via DnsErrorInfo (src/dns/dns_error_info.h), so no
/// parallel DnsError skeleton is defined here.
namespace domain {

/// Static configuration error (shape, values, field combinations).
/// Environment failures (driver not loaded, interface missing) are validated
/// separately and keep throwing ConfigVerificationException until Phase 5.
struct ConfigError {
    enum class Code {
        EMPTY_DOMAIN_NAME,     ///< Domain name must not be empty
        EMPTY_SUBDOMAINS,      ///< Domain must have at least one subdomain
        UPDATE_INTERVAL_LOW,   ///< Interval below the configured minimum
        FORCE_UPDATE_CONFLICT, ///< force_update smaller than update_interval
        EMPTY_SUBDOMAIN_NAME,  ///< Subdomain name must not be empty
        MISSING_INTERFACE,     ///< INTERFACE ip source without interface name
        EMPTY_IP_SOURCE_PARAM, ///< ip_source_param required but empty
        INVALID_IP_SOURCE_URL, ///< HTTP ip source URL has no host/port
        INVALID_MDNS_NAME,     ///< mDNS param is not a valid domain name
        MDNS_NOT_LOCAL,        ///< mDNS param does not end with .local
        MDNS_BAD_RECORD_TYPE,  ///< mDNS source requires type a/aaaa
        INVALID_RESOLVER,      ///< Resolver address is not a valid IP/URI
    };

    Code code;
    std::string message;
};

/// Skeleton (Phase 2): IP source port failure.
struct IpSourceError {
    enum class Code { UNAVAILABLE, NO_ADDRESS, UNKNOWN };
    Code code;
    std::string message;
};

/// Skeleton (Phase 2/4): one driver update attempt failed.
/// `retry_after_seconds` is only meaningful when the driver reported
/// RATE_LIMITED; schedulers ignore it unless Phase 0 approved rescheduling.
struct DriverError {
    enum class Code { UPDATE_FAILED, RATE_LIMITED, CANCELLED, UNKNOWN };
    Code code;
    std::string message;
    int retry_after_seconds{0};
};

/// Skeleton (Phase 3): one update workflow result.
struct UpdateError {
    enum class Code { DRIVER_FAILED, SKIPPED_NO_ADDRESS, UNKNOWN };
    Code code;
    std::string message;
    int retry_after_seconds{0};
};

} // namespace domain

#endif // YADDNSC_DOMAIN_ERROR_H
