//
// Socket error mapping shared by the classic DNS exchanges.
//
// Both exchanges own a Socket and translate its errno values into DNS
// errors; the mapping is identical, so it lives here once.
//

#ifndef YADDNSC_DNS_CLASSIC_SOCKET_ERROR_HPP
#define YADDNSC_DNS_CLASSIC_SOCKET_ERROR_HPP

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <yaddnsc/util/format.hpp>

#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "support/fmt.hpp"

namespace DNS {

/// Map a Socket errno value onto a retry / cancel / fail decision.
[[nodiscard]] inline DnsError classify_socket_error(const int errnum) noexcept {
    if (errnum == ETIMEDOUT || errnum == EAGAIN || errnum == EWOULDBLOCK) {
        return DnsError::RETRY;
    }
    if (errnum == ECANCELED) {
        return DnsError::CANCELLED;
    }
    return DnsError::CONNECTION;
}

/// A Socket errno value with the exchange context that produced it.
///
/// @param transport  "UDP" or "TCP".
/// @param context    The failing step, e.g. "socket", "connect", "recvfrom".
[[nodiscard]] inline DnsErrorInfo exchange_error(const std::uint64_t resolver_id, const char* transport,
                                                 const char* context, const int errnum) {
    return DnsErrorInfo{classify_socket_error(errnum),
                        fmt::format(R"(Resolver #{} {} {}: {})", resolver_id, transport, context,
                                    std::strerror(errnum))};
}

/// True when the call may simply be retried without changing the outcome.
[[nodiscard]] inline bool retryable_io(const int errnum) noexcept {
    return errnum == EINTR || errnum == EAGAIN || errnum == EWOULDBLOCK;
}

}  // namespace DNS

#endif  // YADDNSC_DNS_CLASSIC_SOCKET_ERROR_HPP
