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
/// values inside std::expected, never as exceptions.
///
/// DNS already conforms via DnsErrorInfo (src/dns/dns_error_info.h), so no
/// parallel DnsError skeleton is defined here.
namespace domain {

/// Static configuration error (shape, values, field combinations).
/// Environment failures (driver not loaded, interface missing) are validated
/// separately and keep throwing ConfigVerificationException.
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

/// IP source port failure.
struct IpSourceError {
    enum class Code { UNAVAILABLE, NO_ADDRESS, UNKNOWN };
    Code code;
    std::string message;
};

/// Plugin loading / ABI-contract failure.
///
/// Loading failures surface as PluginError values from the plugin loader and
/// are re-thrown as PluginLoadException at the DriverCatalog boundary (the
/// fail-fast manual-load path); a single update failure is a DriverError.
struct PluginError {
    enum class Code {
        LOAD_FAILED,        ///< dlopen failed (not a loadable module)
        MISSING_SYMBOL,     ///< A required entry point is absent
        ABI_MISMATCH,       ///< Magic number or api_revision mismatch
        CONTRACT_VIOLATION, ///< The plugin violated the ABI contract at runtime
    };
    Code code;
    std::string message;
};

/// Driver update failure (one update attempt through the driver gateway).
/// `retry_after_seconds` is only meaningful when the driver reported
/// RATE_LIMITED; schedulers currently ignore it (no rescheduling).
struct DriverError {
    enum class Code {
        UPDATE_FAILED, ///< Driver executed but reported failure (e.g. upstream rejected)
        NOT_FOUND,     ///< Referenced driver is not loaded
        RATE_LIMITED,  ///< Upstream rate-limited the request (retry_after_seconds set)
        CANCELLED,     ///< Aborted via cancellation
        UNKNOWN,       ///< Any other failure (message carries details)
    };
    Code code;
    std::string message;
    int retry_after_seconds{0};
};

/// One update-workflow failure.
///
/// Every expected failure of a single update cycle surfaces as this value:
///   - SKIPPED_NO_ADDRESS — no usable local address (IP source failed, or no
///     candidate survived the address policy); the cycle is skipped and the
///     schedule carries on;
///   - DRIVER_FAILED — the driver gateway rejected the update (message and,
///     for RATE_LIMITED, retry_after_seconds are copied from DriverError;
///     schedulers currently ignore retry_after);
///   - UNKNOWN — an unexpected exception escaped the workflow (the legacy
///     catch-all boundary, now mapped to an error value).
struct UpdateError {
    enum class Code { DRIVER_FAILED, SKIPPED_NO_ADDRESS, UNKNOWN };
    Code code;
    std::string message;
    int retry_after_seconds{0};
};

} // namespace domain

#endif // YADDNSC_DOMAIN_ERROR_H
