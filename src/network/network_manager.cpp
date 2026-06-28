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
#include <mutex>
#include <stdexcept>
#include <functional>

#include "fmt.hpp"

#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

class NetworkManager::Impl {
public:
    std::vector<std::string> get_interfaces() {
        const auto &interface_map = get_all_interface_addresses();
        std::vector<std::string> interfaces;
        interfaces.reserve(interface_map.size());
        std::ranges::transform(interface_map, std::back_inserter(interfaces), [](const auto &kv) { return kv.first; });

        return interfaces;
    }

    std::map<std::string, int> get_interface_ip_addresses(const std::string &interface_name) {
        const auto &all_interface_addresses = get_all_interface_addresses();
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

    static constexpr auto CACHE_TTL = std::chrono::seconds(5);

    // Returns a const reference to the cached interface map, computing it on
    // first call or after the TTL expires.  Thread-safe: the entire
    // check-then-compute sequence is protected by a single lock, so there is
    // no TOCTOU race and only one thread ever runs collect_interface_addresses().
    const std::map<std::string, std::vector<interface_address> > &get_all_interface_addresses() {
        std::lock_guard lock(cache_mtx_);
        const auto now = Clock::now();
        if (!cached_ || now - last_refresh_ >= CACHE_TTL) {
            cached_interfaces_ = collect_interface_addresses();
            last_refresh_ = now;
            cached_ = true;
        }
        return cached_interfaces_;
    }

    static std::map<std::string, std::vector<interface_address> > collect_interface_addresses() {
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

        return interface_address_map;
    }

    using Clock = std::chrono::steady_clock;

    std::mutex cache_mtx_;
    std::map<std::string, std::vector<interface_address> > cached_interfaces_;
    Clock::time_point last_refresh_;
    bool cached_ = false;
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
