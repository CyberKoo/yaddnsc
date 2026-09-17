//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_NETWORK_SYSTEM_NETWORK_INTERFACES_H
#define YADDNSC_NETWORK_SYSTEM_NETWORK_INTERFACES_H

#include "application/ports/network_interfaces.h"

/// SystemNetworkInterfaces — NetworkInterfaces port implementation over the
/// real OS interface enumeration (InterfaceUtil / getifaddrs).
///
/// Stateless and thread-safe (InterfaceUtil guards its cache internally).
class SystemNetworkInterfaces final : public NetworkInterfaces {
public:
    [[nodiscard]] std::vector<std::string> names() const override;

    /// @throws std::runtime_error  If the interface does not exist.
    [[nodiscard]] std::vector<InetAddress> addresses(const std::string &name) const override;
};

#endif // YADDNSC_NETWORK_SYSTEM_NETWORK_INTERFACES_H
