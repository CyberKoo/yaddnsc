//
// Created by Kotarou on 2026/6/17.
//
#include "network_manager.h"

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <mutex>

#include "fmt.h"

#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

class NetworkManager::Impl {
public:
    std::vector<std::string> get_interfaces() {
        auto interface_map = get_all_interface_addresses();
        std::vector<std::string> interfaces;
        interfaces.reserve(interface_map.size());
        std::ranges::transform(
            interface_map,
            std::back_inserter(interfaces),
            [](const auto &kv) { return kv.first; }
        );

        return interfaces;
    }

    std::map<std::string, int> get_interface_ip_addresses(const std::string &interface_name) {
        auto all_interface_addresses = get_all_interface_addresses();
        if (auto it = all_interface_addresses.find(interface_name); it != all_interface_addresses.end()) {
            std::map<std::string, int> interface_addrs;
            std::ranges::transform(
                it->second, std::inserter(interface_addrs, interface_addrs.end()),
                [](const auto &addr) -> std::pair<std::string, int> {
                    return {addr.address, addr.inet_type};
                }
            );
            return interface_addrs;
        }

        throw std::runtime_error(fmt::format("Interface {} not found", interface_name));
    }

private:
    struct interface_address {
        std::string address;
        int inet_type;
    };

    using ifaddr_ptr = std::unique_ptr<ifaddrs, std::function<void(ifaddrs *)> >;

    static ifaddr_ptr get_ifaddrs() {
        ifaddrs *ifaddr;

        if (getifaddrs(&ifaddr) == -1) {
            throw std::runtime_error("getifaddrs() failed");
        }

        return {ifaddr, [](ifaddrs *a) { freeifaddrs(a); }};
    }

    constexpr static size_t get_address_struct_size(int family) {
        return family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    }

    std::map<std::string, std::vector<interface_address> > get_all_interface_addresses() {
        auto current_time = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(cache_mutex_);
            if (!cached_addresses_.empty() && (current_time - cache_timestamp_) < CACHE_TTL) {
                return cached_addresses_;
            }
        }

        std::map<std::string, std::vector<interface_address> > interface_address_map;
        auto ifaddrs = get_ifaddrs();

        for (auto ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) {
                continue;
            }

            auto family = ifa->ifa_addr->sa_family;
            if (family != AF_INET && family != AF_INET6) {
                continue;
            }

            char host[NI_MAXHOST] = {};
            int ret = getnameinfo(ifa->ifa_addr, get_address_struct_size(family), host, NI_MAXHOST, nullptr, 0,
                                  NI_NUMERICHOST);
            if (ret != 0) {
                auto err_msg = fmt::format("getnameinfo() failed, error: {}", gai_strerror(ret));
                if (ret == EAI_SYSTEM) {
                    err_msg += fmt::format(" (errno: {})", strerror(errno));
                }
                throw std::runtime_error(err_msg);
            }

            interface_address_map[ifa->ifa_name].emplace_back(interface_address{host, family});
        }

        {
            std::lock_guard lock(cache_mutex_);
            cached_addresses_ = interface_address_map;
            cache_timestamp_ = current_time;
        }
        return interface_address_map;
    }

    static constexpr auto CACHE_TTL = std::chrono::seconds(5);

    std::mutex cache_mutex_;
    std::map<std::string, std::vector<interface_address> > cached_addresses_;
    std::chrono::steady_clock::time_point cache_timestamp_;
};

NetworkManager::NetworkManager() : impl_(std::make_unique<Impl>()) {
}

NetworkManager::~NetworkManager() = default;

std::vector<std::string> NetworkManager::get_interfaces() const {
    return impl_->get_interfaces();
}

std::map<std::string, int> NetworkManager::get_interface_ip_addresses(const std::string &interface_name) const {
    return impl_->get_interface_ip_addresses(interface_name);
}
