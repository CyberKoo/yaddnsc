//
// Created by Kotarou on 2022/4/5.
//
#include "network_util.h"

#include <map>
#include <vector>
#include <string>
#include <fmt/format.h>

#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

std::vector<std::string> NetworkUtil::get_interfaces() {
    std::vector<std::string> interfaces;

    auto if_addrs = get_all_ip_addresses();
    interfaces.reserve(if_addrs.size());
    std::transform(if_addrs.begin(), if_addrs.end(), std::back_inserter(interfaces),
                   [](const auto &kv) { return kv.first; });

    return interfaces;
}

std::map<std::string, int> NetworkUtil::get_nif_ip_address(std::string_view nif) {
    auto all_nif_addrs = get_all_ip_addresses();
    if (all_nif_addrs.find(nif.data()) != all_nif_addrs.end()) {
        auto ip_addresses = all_nif_addrs[nif.data()];

        auto nif_ip_addrs = std::map<std::string, int>();
        std::transform(ip_addresses.begin(), ip_addresses.end(), std::inserter(nif_ip_addrs, nif_ip_addrs.end()),
                       [](const auto &addr) {
                           return std::pair<std::string, int>{addr.address, addr.inet_type};
                       }
        );

        return nif_ip_addrs;

    } else {
        throw std::runtime_error(fmt::format("Interface {} not found", nif));
    }
}

std::map<std::string, std::vector<NetworkUtil::interface_addrs_t>> NetworkUtil::get_all_ip_addresses() {
    std::map<std::string, std::vector<interface_addrs_t>> address_map;

    auto ifaddrs = get_ifaddrs();
    for (auto ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr != nullptr) {
            // ignore address not ipv6 or ipv4
            if (ifa->ifa_addr->sa_family != AF_INET and ifa->ifa_addr->sa_family != AF_INET6) {
                continue;
            }

            if (address_map.find(ifa->ifa_name) == address_map.end()) {
                address_map.insert({ifa->ifa_name, {}});
            }

            char host[NI_MAXHOST];
            int error = getnameinfo(ifa->ifa_addr, get_address_struct_size(ifa->ifa_addr->sa_family), host,
                                    NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (error != 0) {
                throw std::runtime_error(fmt::format("getnameinfo() failed: {}", gai_strerror(error)));
            }

            address_map[ifa->ifa_name].emplace_back(interface_addrs_t{host, ifa->ifa_addr->sa_family});

        }
    }

    return address_map;
}

NetworkUtil::ifaddrs_ptr_t NetworkUtil::get_ifaddrs() {
    struct ifaddrs *ifaddr;

    if (getifaddrs(&ifaddr) == -1) {
        throw std::runtime_error("getifaddrs() error");
    }

    return {ifaddr, [](ifaddrs *a) { freeifaddrs(a); }};
}

size_t NetworkUtil::get_address_struct_size(int family) {
    if (family == AF_INET6) {
        return sizeof(struct sockaddr_in6);
    } else {
        return sizeof(struct sockaddr_in);
    }
}
