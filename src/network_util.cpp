//
// Created by Kotarou on 2022/4/5.
//
#include "network_util.h"

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <functional>

#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

struct interface_addrs {
    std::string address;
    int inet_type;
};

using ifaddrs_ptr_t = std::unique_ptr<ifaddrs, std::function<void(ifaddrs *)>>;

ifaddrs_ptr_t get_ifaddrs();

std::map<std::string, std::vector<interface_addrs>> get_all_ip_addresses();

template<typename T, typename = std::enable_if_t<std::is_trivial_v<T>>, typename = std::enable_if_t<!std::is_pointer_v<T>>>
size_t calc_obj_size(const T &);

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
                       [](const auto &addr) -> std::pair<std::string, int> {
                           return {addr.address, addr.inet_type};
                       }
        );

        return nif_ip_addrs;

    } else {
        throw std::runtime_error(std::string("Interface ") + nif.data() + " not found");
    }
}

std::map<std::string, std::vector<interface_addrs>> get_all_ip_addresses() {
    std::map<std::string, std::vector<interface_addrs>> address_map;

    auto ifaddrs = get_ifaddrs();
    for (auto ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr != nullptr) {
            // ignore address not ipv6 or ipv4
            if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6) {
                continue;
            }

            if (address_map.find(ifa->ifa_name) == address_map.end()) {
                address_map.insert({ifa->ifa_name, {}});
            }

            char host[NI_MAXHOST] = {};
            int error = getnameinfo(ifa->ifa_addr, calc_obj_size(ifa->ifa_addr->sa_family), host,
                                    NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (error != 0) {
                throw std::runtime_error(std::string("getnameinfo() failed, error: ") + gai_strerror(error));
            }

            address_map[ifa->ifa_name].emplace_back(interface_addrs{host, ifa->ifa_addr->sa_family});
        }
    }

    return address_map;
}

ifaddrs_ptr_t get_ifaddrs() {
    struct ifaddrs *ifaddr;

    if (getifaddrs(&ifaddr) == -1) {
        throw std::runtime_error("getifaddrs() error");
    }

    return {ifaddr, [](ifaddrs *a) { freeifaddrs(a); }};
}

template<typename T, typename, typename>
size_t calc_obj_size(const T &obj) {
    return sizeof(obj);
}
