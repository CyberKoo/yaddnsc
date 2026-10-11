#ifndef YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP
#define YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP

#include <optional>
#include <string>
#include <utility>

#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/types.h"

/// DNS utility — compile-time type conversion helpers.
namespace dns::Util {
/// Convert RecordKind to the corresponding wire-format RecordType.
///
/// @param kind  The RecordKind from the updater layer.
/// @return      The corresponding RecordType for wire-format construction.
[[nodiscard]] constexpr RecordType type_to_record_type(domain::RecordKind kind) noexcept {
    switch (kind) {
        case domain::RecordKind::A:
            return RecordType::A;
        case domain::RecordKind::AAAA:
            return RecordType::AAAA;
        case domain::RecordKind::TXT:
            return RecordType::TXT;
    }
    // All RecordKind enumerators are handled above.  The trailing
    // std::unreachable() suppresses -Werror=return-type.  If reached
    // (e.g. a new enumerator was added without updating this switch),
    // -Wswitch will fire because there is no default label.
    std::unreachable();
}

/// Classify a server's RCODE consistently at every DNS entry point.
/// NOERROR still requires the caller to check for usable records.
[[nodiscard]] inline std::optional<domain::DnsErrorInfo> rcode_error(Rcode rcode, const std::string& host) {
    const auto prefix = "DNS lookup for \"" + host + "\"";
    switch (rcode) {
        case Rcode::NOERROR:
            return std::nullopt;
        case Rcode::NXDOMAIN:
            return domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, prefix + " returned NXDOMAIN"};
        case Rcode::SERVFAIL:
            return domain::DnsErrorInfo{domain::DnsError::RETRY, prefix + " returned SERVFAIL"};
        case Rcode::REFUSED:
            return domain::DnsErrorInfo{domain::DnsError::SERVER_REFUSED, prefix + " was refused"};
        default:
            return domain::DnsErrorInfo{domain::DnsError::UNKNOWN,
                                        prefix + " returned RCODE " + std::to_string(std::to_underlying(rcode))};
    }
}

/// An incomplete/failed view must not be reported as a definitive negative.
/// Among negative answers, NODATA is stronger evidence of existence than NXDOMAIN.
[[nodiscard]] constexpr int error_priority(domain::DnsError code) noexcept {
    switch (code) {
        case domain::DnsError::INVALID_QUERY:
            return 6;
        case domain::DnsError::CONFIG:
            return 5;
        case domain::DnsError::PARSE:
            return 4;
        case domain::DnsError::NODATA:
            return 2;
        case domain::DnsError::NX_DOMAIN:
            return 1;
        default:
            return 3;
    }
}
}  // namespace dns::Util

#endif  // YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP
