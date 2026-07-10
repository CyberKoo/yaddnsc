//
// Created by Kotarou on 2026/7/11.
//

#ifndef YADDNSC_DNS_ERROR_INFO_H
#define YADDNSC_DNS_ERROR_INFO_H

#include <string>
#include <string_view>

#include "dns_error.h"

/// Lightweight value type for std::expected error reporting.
///
/// Unlike DnsLookupException (which is an exception class for terminal
/// signals), DnsErrorInfo is a plain value type suitable for use as the
/// E parameter of std::expected<T, E>.  It carries a DnsError code and
/// a human-readable message.
///
/// Regular value semantics: copyable, movable, trivially destructible-ish.
struct DnsErrorInfo {
    DnsError code{DnsError::UNKNOWN};
    std::string message;

    [[nodiscard]] std::string_view error_name() const noexcept {
        return error_to_str(code);
    }
};

#endif  // YADDNSC_DNS_ERROR_INFO_H
