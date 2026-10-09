#ifndef YADDNSC_DOMAIN_ERROR_ERROR_H
#define YADDNSC_DOMAIN_ERROR_ERROR_H

#include <string>

/// Domain-level error types.
///
/// Shared convention: every error carries a category-specific `code` plus a
/// human-readable `message`. Recoverable errors cross layer boundaries as
/// values inside std::expected, never as exceptions.
///
/// DNS already conforms via DnsErrorInfo (src/domain/error/dns_error_info.h), so no
/// parallel DnsError skeleton is defined here.
namespace domain {

/// Configuration error. `Code` covers both static checks (shape, values,
/// field combinations) and environment checks (driver loaded, interface
/// present); the environment checks are performed against ports by the
/// application-level environment validator.
struct ConfigError {
    enum class Code {
        EMPTY_DOMAINS,          ///< Config must define at least one domain
        EMPTY_DOMAIN_NAME,      ///< Domain name must not be empty
        EMPTY_SUBDOMAINS,       ///< Domain must have at least one subdomain
        UPDATE_INTERVAL_LOW,    ///< Interval below the configured minimum
        FORCE_UPDATE_CONFLICT,  ///< force_update smaller than update_interval
        EMPTY_SUBDOMAIN_NAME,   ///< Subdomain name must not be empty
        MISSING_INTERFACE,      ///< INTERFACE ip source without interface name
        EMPTY_IP_SOURCE_PARAM,  ///< ip_source_param required but empty
        INVALID_IP_SOURCE_URL,  ///< HTTP ip source URL has no host/port
        INVALID_MDNS_NAME,      ///< mDNS param is not a valid domain name
        MDNS_NOT_LOCAL,         ///< mDNS param does not end with .local
        MDNS_BAD_RECORD_TYPE,   ///< mDNS source requires type a/aaaa
        INVALID_RESOLVER,       ///< Resolver address is not a valid IP/URI
        NO_RESOLVER_SERVERS,    ///< use_custom_servers set but no servers configured
        INVALID_BOOTSTRAP_DNS,  ///< bootstrap_dns is not a valid IP literal
        DRIVER_NOT_FOUND,       ///< Referenced driver plugin is not loaded
        INTERFACE_NOT_FOUND,    ///< Referenced network interface does not exist
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

/// Driver lookup, configuration validation, or update failure.
/// `retry_after_seconds` is preserved whenever an upstream or transport
/// failure supplies it; the update workflow reports it back to its subdomain
/// loop, which moves the task's next deadline to honour the backoff.
struct DriverError {
    enum class Code {
        UPDATE_FAILED,  ///< Update was not applied (e.g. upstream rejected, or the host refused the record type)
        NOT_FOUND,      ///< Referenced driver is not loaded
        RATE_LIMITED,   ///< Upstream rate-limited the request
        UNKNOWN,        ///< Any other failure (message carries details)
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
///     retry_after_seconds are copied from DriverError; the subdomain loop
///     feeds retry_after into the next delay for backoff rescheduling);
///   - UNKNOWN — an unexpected exception escaped the workflow (the legacy
///     catch-all boundary, now mapped to an error value).
struct UpdateError {
    enum class Code { DRIVER_FAILED, SKIPPED_NO_ADDRESS, UNKNOWN };
    Code code;
    std::string message;
    int retry_after_seconds{0};
};

}  // namespace domain

#endif  // YADDNSC_DOMAIN_ERROR_ERROR_H
