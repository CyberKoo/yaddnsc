//
// Created by Kotarou on 2026/7/4.
//

#ifndef YADDNSC_DNS_ERROR_H
#define YADDNSC_DNS_ERROR_H

#include <string_view>

// ---------------------------------------------------------------------------
// DNS error types — shared error enum and stringification.
// ---------------------------------------------------------------------------
namespace DNS {

    // NOTE: NX_DOMAIN (with underscore) is intentional. The system header
    // <arpa/nameser.h> defines NXDOMAIN as ns_r_nxdomain, which would
    // cause a preprocessor conflict if we used the bare NXDOMAIN name.
    enum class Error {
        NX_DOMAIN, RETRY, NODATA, PARSE, CONNECTION, UNKNOWN
    };

    [[nodiscard]] std::string_view error_to_str(Error error);

} // namespace DNS

#endif // YADDNSC_DNS_ERROR_H
