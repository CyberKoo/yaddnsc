//
// Created by Kotarou on 2026/7/1.
//

#include "iface.h"

#include <vector>

#include "iface_util.h"
#include "network/inet_address.h"

InterfaceIpSource::InterfaceIpSource(std::string interface_name, AddressFamily address_family)
    : interface_name_(std::move(interface_name)), address_family_(address_family) {
}

std::vector<InetAddress> InterfaceIpSource::resolve() const {
    auto addresses = InterfaceUtil::get_addresses(interface_name_);

    // Filter by address family.
    if (address_family_ != AddressFamily::UNSPECIFIED) {
        std::erase_if(addresses, [af = address_family_](const InetAddress &addr) {
            return addr.get_family() != af;
        });
    }

    return addresses;
}
