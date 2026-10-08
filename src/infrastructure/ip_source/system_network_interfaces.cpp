//
// Created by Kotarou on 2026/9/17.
//

#include "system_network_interfaces.h"

#include <vector>

#include "infrastructure/ip_source/iface_util.h"

std::vector<std::string> SystemNetworkInterfaces::names() const {
    return ipsource::get_interfaces();
}

std::optional<std::vector<InetAddress>> SystemNetworkInterfaces::addresses(const std::string& name) const {
    return ipsource::get_addresses(name);
}
