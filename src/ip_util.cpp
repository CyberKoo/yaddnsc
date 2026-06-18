//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

bool IPUtil::is_ipv6_local_link(const std::string &ip_addr) {
    sockaddr_in6 sa{};
    if (inet_pton(AF_INET6, ip_addr.data(), &sa.sin6_addr) != 1) {
        return true; // unparseable → treat as link-local (don't use)
    }

    auto is_local = [](const in6_addr *addr) {
        return addr->s6_addr[0] == 0xfe && (addr->s6_addr[1] & 0xc0) == 0x80;
    };

    auto is_site_local = [](const in6_addr *addr) {
        return addr->s6_addr[0] == 0xfe && (addr->s6_addr[1] & 0xc0) == 0xc0;
    };

    return is_local(&sa.sin6_addr) || is_site_local(&sa.sin6_addr);
}

bool IPUtil::is_ipv6_ula(const std::string &ip_addr) {
    sockaddr_in6 sa{};
    if (inet_pton(AF_INET6, ip_addr.data(), &(sa.sin6_addr)) != 1) {
        return false;
    }

    return sa.sin6_addr.s6_addr[0] == 0xfc || sa.sin6_addr.s6_addr[0] == 0xfd;
}
