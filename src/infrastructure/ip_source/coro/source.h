//
// ip_source — coroutine IP sources.
//
// One coroutine interface per source; the sync-vs-offload choice is made per
// source (see iface/http/mdns). Compatibility note: these reuse the legacy
// InterfaceUtil cache and, for mDNS, the legacy synchronous source behind
// coro::offload — that is a deliberate transition debt for stage 3.
//

#ifndef YADDNSC_IP_SOURCE_CORO_SOURCE_H
#define YADDNSC_IP_SOURCE_CORO_SOURCE_H

#include <vector>

#include <expected>

#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"

namespace ipsource {

/// Result of one source lookup: candidate addresses, or a structured failure.
using Result = std::expected<std::vector<InetAddress>, domain::IpSourceError>;

/// One coroutine IP source.
///
/// Cancellation: each await is a scope checkpoint; a cancelled scope yields an
/// IpSourceError with Code::CANCELLED.
/// Thread safety: loop thread only.
class CoroIpSource {
public:
    virtual ~CoroIpSource() = default;

    [[nodiscard]] virtual coro::Task<Result> resolve() = 0;
};

}  // namespace ipsource

#endif  // YADDNSC_IP_SOURCE_CORO_SOURCE_H
