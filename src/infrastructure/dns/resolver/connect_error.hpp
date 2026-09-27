//
// Shared connect-stage error mapping for the DNS resolvers (DoH / DoT).
//

#ifndef YADDNSC_DNS_RESOLVER_CONNECT_ERROR_HPP
#define YADDNSC_DNS_RESOLVER_CONNECT_ERROR_HPP

#include <string_view>

#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/network/transport/io_error.h"
#include "support/fmt.hpp"

namespace DNS::Resolver {
/// Map a transport I/O error to DnsErrorInfo (connect stage).
[[nodiscard]] inline DnsErrorInfo map_connect_error(const Transport::IoError err, const std::string_view label) {
    using enum Transport::IoError;
    switch (err) {
        case CANCELLED:
            return {DnsError::CANCELLED, "Query cancelled"};
        case TIMEOUT:
            return {DnsError::RETRY, fmt::format(R"(Connection to "{}" timed out)", label)};
        case CONNECTION_FAILED:
            return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
    }
    return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
}
}  // namespace DNS::Resolver

#endif  // YADDNSC_DNS_RESOLVER_CONNECT_ERROR_HPP
