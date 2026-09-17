//
// Created by Kotarou on 2026/9/17.
//

#include "system_network_interfaces.h"

#include "ip_source/iface_util.h"

std::vector<std::string> SystemNetworkInterfaces::names() const {
    return InterfaceUtil::get_interfaces();
}

std::vector<InetAddress> SystemNetworkInterfaces::addresses(const std::string &name) const {
    return InterfaceUtil::get_addresses(name);
}
