#include "system_network_interfaces.h"

#include <vector>
#include <optional>

#include "infrastructure/ip_source/iface_util.h"

std::vector<std::string> SystemNetworkInterfaces::names() const {
    return ipsource::get_interfaces();
}

std::optional<std::vector<domain::InetAddress>> SystemNetworkInterfaces::addresses(const std::string& name) const {
    return ipsource::get_addresses(name);
}

std::vector<app::InterfaceInfo> SystemNetworkInterfaces::list() const {
    std::vector<app::InterfaceInfo> items;
    for (auto& row : ipsource::get_all()) {
        items.push_back(app::InterfaceInfo{.name = std::move(row.name), .addresses = std::move(row.addresses)});
    }
    return items;
}
