//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_UTIL_H
#define YADDNSC_DNS_UTIL_H

#include <arpa/nameser.h>

#include "dns_type.h"
#include "address_family.h"

// ---------------------------------------------------------------------------
// DNS utility — compile-time type conversion helpers.
// ---------------------------------------------------------------------------
namespace DNS {

    /// Convert DNS record type to address family (A → IPV4, AAAA → IPV6).
    constexpr AddressFamily type_to_family(Type type) noexcept {
        switch (type) {
            case Type::A:    return AddressFamily::IPV4;
            case Type::AAAA: return AddressFamily::IPV6;
            default:         return AddressFamily::UNSPECIFIED;
        }
    }

    /// Convert to the corresponding <arpa/nameser.h> ns_t_* constant.
    constexpr int to_ns_type(Type type) noexcept {
        switch (type) {
            case Type::A:    return ns_t_a;
            case Type::AAAA: return ns_t_aaaa;
            case Type::TXT:  return ns_t_txt;
            case Type::SOA:  return ns_t_soa;
            default:         return ns_t_invalid;
        }
    }

} // namespace DNS

#endif // YADDNSC_DNS_UTIL_H
