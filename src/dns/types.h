//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_TYPES_H
#define YADDNSC_DNS_TYPES_H

#include <string_view>

#include "type.h"

// ---------------------------------------------------------------------------
// DNS utility namespace — purely static helper functions.
//
// Separated from the resolver classes so that any code that needs enum
// conversions can include this lightweight header without pulling in the
// full resolver hierarchy.
// ---------------------------------------------------------------------------
namespace DNS {
    std::string_view error_to_str(dns_error_type error);

    address_family_type dns2ip(dns_type type);

    // Convert dns_type to the corresponding <arpa/nameser.h> ns_t_* constant.
    [[nodiscard]] int to_ns_type(dns_type type) noexcept;
}

#endif // YADDNSC_DNS_TYPES_H
