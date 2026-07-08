//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_UTIL_H
#define YADDNSC_DNS_UTIL_H

#include <utility>

#include "record_kind.h"
#include "dns/types.h"

/// DNS utility — compile-time type conversion helpers.
namespace DNS::Util {

    /// Convert RecordKind to the corresponding wire-format RecordType.
    ///
    /// @param kind  The RecordKind from the updater layer.
    /// @return      The corresponding RecordType for wire-format construction.
    [[nodiscard]] constexpr RecordType type_to_record_type(RecordKind kind) noexcept {
        switch (kind) {
            case RecordKind::A:    return RecordType::A;
            case RecordKind::AAAA: return RecordType::AAAA;
            case RecordKind::TXT:  return RecordType::TXT;
        }
        // All RecordKind enumerators are handled above.  The trailing
        // std::unreachable() suppresses -Werror=return-type.  If reached
        // (e.g. a new enumerator was added without updating this switch),
        // -Wswitch will fire because there is no default label.
        std::unreachable();
    }

} // namespace DNS::Util

#endif  // YADDNSC_DNS_UTIL_H
