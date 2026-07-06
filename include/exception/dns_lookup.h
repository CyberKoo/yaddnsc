//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_EXCEPTION_DNS_LOOKUP_H
#define YADDNSC_EXCEPTION_DNS_LOOKUP_H

#include "base.h"
#include "dns_error.h"

/// Thrown when a DNS lookup fails.
///
/// Carries a typed DNS::Error code in addition to the human-readable message,
/// allowing callers to handle transient errors (RETRY) differently from
/// permanent ones (NX_DOMAIN, NODATA).
class DnsLookupException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    /// Construct with a message and a typed error code.
    DnsLookupException(const std::string &msg, DNS::Error err) : YaddnscException(msg), error_(err) {
    }

    /// @overload
    DnsLookupException(const char *msg, DNS::Error err) : YaddnscException(msg), error_(err) {
    }

    /// Construct by wrapping another exception with a DNS error code.
    DnsLookupException(YaddnscException &&exc, DNS::Error err) : YaddnscException(exc), error_(err) {
    }

    /// @overload
    DnsLookupException(const YaddnscException &exc, DNS::Error err) : YaddnscException(exc), error_(err) {
    }

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "DnsLookupException";
    }

    /// Return the typed DNS error code associated with this exception.
    [[nodiscard]] DNS::Error get_error() const noexcept {
        return error_;
    }

private:
    DNS::Error error_; ///< Categorised DNS error code
};


#endif //YADDNSC_EXCEPTION_DNS_LOOKUP_H
