#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_SYSTEM_NETWORK_INTERFACES_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_SYSTEM_NETWORK_INTERFACES_H

#include <string>

#include "application/ports/network_interfaces.h"

/// SystemNetworkInterfaces — app::NetworkInterfacesPort port implementation over the
/// real OS interface enumeration (InterfaceUtil / getifaddrs).
///
/// Stateless and thread-safe: each call reads a live getifaddrs() snapshot;
/// InterfaceUtil keeps no shared cache.
class SystemNetworkInterfaces final : public app::NetworkInterfacesPort {
public:
    [[nodiscard]] std::vector<std::string> names() const override;

    /// std::nullopt when the interface does not exist.
    [[nodiscard]] std::optional<std::vector<domain::InetAddress>> addresses(const std::string& name) const override;

    /// One getifaddrs() pass for the whole listing.
    [[nodiscard]] std::vector<app::InterfaceInfo> list() const override;
};

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_SYSTEM_NETWORK_INTERFACES_H
