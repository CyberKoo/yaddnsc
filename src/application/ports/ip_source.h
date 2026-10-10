#ifndef YADDNSC_APPLICATION_PORTS_IP_SOURCE_H
#define YADDNSC_APPLICATION_PORTS_IP_SOURCE_H

#include <vector>

#include <expected>

#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "coro/task.hpp"

namespace domain {
struct SubdomainConfig;
}

namespace app {

/// Obtain local address candidates for a subdomain.
///
/// Failure: IpSourceError values; cancellation propagates as `coro::Cancelled`.
/// An empty candidate list is a success. The port and borrowed configuration
/// must outlive the task. Thread safety: loop-thread only.
class IpSourcePort {
public:
    virtual ~IpSourcePort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_IP_SOURCE_H
