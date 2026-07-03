//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_UTIL_H
#define YADDNSC_DNS_UTIL_H

#include <string>
#include <string_view>

#include "dns_type.h"
#include "address_family.h"

// ---------------------------------------------------------------------------
// DNS utility namespace — DNS type definitions and helper functions.
// ---------------------------------------------------------------------------
namespace DNS {

    // NOTE: NX_DOMAIN (with underscore) is intentional. The system header
    // <arpa/nameser.h> defines NXDOMAIN as ns_r_nxdomain, which would
    // cause a preprocessor conflict if we used the bare NXDOMAIN name.
    enum class Error {
        NX_DOMAIN, RETRY, NODATA, PARSE, CONNECTION, UNKNOWN
    };

    std::string_view error_to_str(Error error);

    // Convert DNS record type to address family (A → IPV4, AAAA → IPV6).
    AddressFamily type_to_family(Type type);

    // Convert to the corresponding <arpa/nameser.h> ns_t_* constant.
    [[nodiscard]] int to_ns_type(Type type) noexcept;

} // namespace DNS

#endif // YADDNSC_DNS_UTIL_H
