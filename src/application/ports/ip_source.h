//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_IP_SOURCE_H
#define YADDNSC_APPLICATION_PORTS_IP_SOURCE_H

#include <expected>
#include <vector>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"

namespace Utils {
class CancellationToken;
}

/// IpSourcePort — application port for obtaining local IP address candidates.
///
/// Implementations create the configured source (interface / HTTP / mDNS),
/// run it, and translate failures into IpSourceError values.
///
/// Result contract:
///   - failure is an error value (the legacy implementations throw; the
///     adapter converts — application code never sees those exceptions);
///   - an empty candidate list is a SUCCESS, not an error ("the source worked
///     but found no matching address");
///   - which candidate (if any) gets published is decided by
///     domain::select_address in the workflow, not by this port.
///
/// Cancellation flows through resolve() as a parameter
/// (Utils::CancellationToken), derived from the process root source.
class IpSourcePort {
public:
    virtual ~IpSourcePort() = default;

    /// Resolve the local address candidates for a subdomain configuration.
    [[nodiscard]] virtual std::expected<std::vector<InetAddress>, domain::IpSourceError>
    resolve(const domain::SubdomainConfig &config, const Utils::CancellationToken &token) const = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_IP_SOURCE_H
