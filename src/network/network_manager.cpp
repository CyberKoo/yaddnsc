//
// Created by Kotarou on 2026/6/17.
//
#include "network_manager.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <functional>

#include "fmt.hpp"

#include "inet_address.h"

class NetworkManager::Impl {
public:
    std::vector<std::string> get_interfaces() {
        const auto &interface_map = get_all_interface_addresses();
        std::vector<std::string> interfaces;
        interfaces.reserve(interface_map.size());
        std::ranges::transform(interface_map, std::back_inserter(interfaces), [](const auto &kv) { return kv.first; });

        return interfaces;
    }

    std::vector<InetAddress> get_interface_ip_addresses(const std::string &interface_name) {
        const auto &all_interface_addresses = get_all_interface_addresses();
        if (auto it = all_interface_addresses.find(interface_name); it != all_interface_addresses.end()) {
            std::vector<InetAddress> result;
            for (const auto &entry: it->second) {
                if (auto parsed = InetAddress::from_bytes(entry.addr_bytes)) {
                    result.push_back(std::move(*parsed));
                }
            }
            return result;
        }

        throw std::runtime_error(fmt::format("Interface {} not found", interface_name));
    }

private:
    struct interface_address {
        std::vector<uint8_t> addr_bytes;
    };

    using ifaddr_ptr = std::unique_ptr<ifaddrs, std::function<void(ifaddrs *)> >;

    static ifaddr_ptr get_ifaddrs() {
        ifaddrs *ifaddr;

        if (getifaddrs(&ifaddr) == -1) {
            throw std::runtime_error("getifaddrs() failed");
        }

        return {ifaddr, [](ifaddrs *a) { freeifaddrs(a); }};
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

            std::vector<uint8_t> bytes;
            if (family == AF_INET) {
                const auto *in = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
                const auto *raw = reinterpret_cast<const uint8_t *>(&in->sin_addr.s_addr);
                bytes.assign(raw, raw + 4);
            } else {
                const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(ifa->ifa_addr);
                bytes.assign(in6->sin6_addr.s6_addr, in6->sin6_addr.s6_addr + 16);
            }

            interface_address_map[ifa->ifa_name].emplace_back(interface_address{std::move(bytes)});
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

std::vector<InetAddress> NetworkManager::get_interface_ip_addresses(const std::string &interface_name) const {
    return impl_->get_interface_ip_addresses(interface_name);
}
