//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <vector>
#include <sys/socket.h>

#include <httplib.h>

#include "type.h"
#include "network_manager.h"

std::vector<std::string> IPUtil::get_ip_from_interface(NetworkManager &mgr, const std::string &nif, address_family af) {
    auto addresses = mgr.get_nif_ip_address(nif);
    std::vector<std::string> nif_addresses;
    for (auto &[ip_address, family]: addresses) {
        if (af == address_family::UNSPECIFIED || family == ip2af(af)) {
            nif_addresses.emplace_back(ip_address);
        }
    }

    return nif_addresses;
}

int IPUtil::ip2af(address_family version) {
    switch (version) {
        case address_family::IPV4:
            return AF_INET;
        case address_family::IPV6:
            return AF_INET6;
        default:
            return AF_UNSPEC;
    }
}

bool IPUtil::is_ipv4_address(const std::string &str) {
    sockaddr_in sa{};
    return inet_pton(AF_INET, str.c_str(), &(sa.sin_addr)) != 0;
}

bool IPUtil::is_ipv6_address(const std::string &str) {
    sockaddr_in6 sa{};
    return inet_pton(AF_INET6, str.c_str(), &(sa.sin6_addr)) != 0;
}
