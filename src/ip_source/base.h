//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_BASE_H
#define YADDNSC_IP_SOURCE_BASE_H

#include <vector>

#include "mixin.h"
#include "network/inet_address.h"

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
    virtual ~IpSourceBase() = default;

    IpSourceBase() = default;

    IpSourceBase(IpSourceBase &&) noexcept = default;

    IpSourceBase &operator=(IpSourceBase &&) noexcept = default;

    /// Resolve the local IP address(es).
    ///
    /// For sources that return multiple candidates (interface, mDNS), all found
    /// addresses are returned so the caller can apply policy filters.
    ///
    /// @return  A vector of resolved addresses, or empty on failure.
    [[nodiscard]] virtual std::vector<InetAddress> resolve() const = 0;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
};

#endif // YADDNSC_IP_SOURCE_BASE_H
