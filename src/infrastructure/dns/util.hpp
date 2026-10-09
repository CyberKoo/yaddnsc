#ifndef YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP
#define YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP

#include <utility>

#include "domain/dns/record_kind.h"
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
}  // namespace dns::Util

#endif  // YADDNSC_INFRASTRUCTURE_DNS_UTIL_HPP
