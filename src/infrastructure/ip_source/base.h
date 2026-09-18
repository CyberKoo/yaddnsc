//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_BASE_H
#define YADDNSC_IP_SOURCE_BASE_H

#include <expected>
#include <vector>

#include "domain/error/error.h"
#include "domain/network/inet_address.h"

#include "support/mixin.h"

namespace Utils {
class CancellationToken;
}

/// IpSourceBase — abstract interface for obtaining a local IP address.
///
/// Three concrete implementations exist:
///   - InterfaceIpSource — reads addresses from a local network interface
///   - HttpIpSource      — fetches the address from an external HTTP service
///   - MdnsIpSource      — discovers a LAN device via mDNS multicast
///
/// @note Thread-safe: resolve() is const and does not mutate shared state.
class IpSourceBase {
public:
    using Result = std::expected<std::vector<InetAddress>, domain::IpSourceError>;

    virtual ~IpSourceBase() = default;

    IpSourceBase() = default;

    IpSourceBase(IpSourceBase &&) noexcept = default;

    IpSourceBase &operator=(IpSourceBase &&) noexcept = default;

    /// Resolve the local IP address(es).
    ///
    /// For sources that return multiple candidates (interface, mDNS), all found
    /// addresses are returned so the caller can apply policy filters.
    ///
    /// @param token  Cancellation token observed by blocking I/O (HTTP / mDNS
    ///               sources); sources without blocking I/O ignore it.
    /// @return Resolved addresses (possibly empty) or a structured source
    ///         failure. An empty vector is successful resolution with no
    ///         matching address; cancellation is Code::CANCELLED.
    [[nodiscard]] virtual Result resolve(const Utils::CancellationToken& token) const = 0;

private:
    [[maybe_unused, no_unique_address]] NoCopy no_copy_;
};

#endif  // YADDNSC_IP_SOURCE_BASE_H
