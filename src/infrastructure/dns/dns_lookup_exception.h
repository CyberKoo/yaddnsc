#ifndef YADDNSC_INFRASTRUCTURE_DNS_DNS_LOOKUP_EXCEPTION_H
#define YADDNSC_INFRASTRUCTURE_DNS_DNS_LOOKUP_EXCEPTION_H

#include "domain/error/dns_error.h"
#include "support/exception.h"

/// Thrown when a DNS lookup fails.
///
/// Carries a typed DnsError code in addition to the human-readable message,
/// allowing callers to handle transient errors (RETRY) differently from
/// permanent ones (NX_DOMAIN, NODATA).
class DnsLookupException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    /// Construct with a message and a typed error code.
    DnsLookupException(const std::string& msg, domain::DnsError err) : YaddnscException(msg), error_(err) {}

    /// @overload
    DnsLookupException(const char* msg, domain::DnsError err) : YaddnscException(msg), error_(err) {}

    /// Construct by wrapping another exception with a DNS error code.
    DnsLookupException(YaddnscException&& exc, domain::DnsError err) : YaddnscException(exc), error_(err) {}

    /// @overload
    DnsLookupException(const YaddnscException& exc, domain::DnsError err) : YaddnscException(exc), error_(err) {}

    [[nodiscard]] std::string_view get_name() const noexcept override { return "DnsLookupException"; }

    /// Return the typed DNS error code associated with this exception.
    [[nodiscard]] domain::DnsError get_error() const noexcept { return error_; }

private:
    domain::DnsError error_{domain::DnsError::UNKNOWN};  ///< Categorised DNS error code
};


#endif  // YADDNSC_INFRASTRUCTURE_DNS_DNS_LOOKUP_EXCEPTION_H
