//
// Created by Kotarou on 2026/7/1.
//

#include "iface_util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "domain/network/inet_address.h"
#include "support/util/cache.hpp"

// ===========================================================================
// Internal enumeration + cache
// ===========================================================================

namespace {
using InterfaceMap = std::map<std::string, std::vector<InetAddress>>;

/// RAII deleter for the getifaddrs() linked list.
using IfAddrPtr = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>;

[[nodiscard]] IfAddrPtr query_ifaddrs() {
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == -1) {
        throw std::runtime_error("getifaddrs() failed");
    }
    return {ifa, &freeifaddrs};
}

/// Enumerate every interface that carries an IPv4 or IPv6 address. IPv6
/// link-local scope IDs are preserved.
[[nodiscard]] InterfaceMap enumerate_interfaces() {
    InterfaceMap result;
    auto ifaddrs = query_ifaddrs();

    for (auto* ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }
        const auto family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
            Inet4Address::addr_type array{};
            const auto bytes =
                std::span{reinterpret_cast<const std::uint8_t*>(&in->sin_addr.s_addr), Inet4Address::ADDR_LEN};
            std::ranges::copy(bytes, array.begin());
            result[ifa->ifa_name].emplace_back(Inet4Address::from_bytes(array));
        } else if (family == AF_INET6) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
            Inet6Address::addr_type array{};
            std::ranges::copy(in6->sin6_addr.s6_addr, array.begin());
            auto address = Inet6Address::from_bytes(array);
            address.set_scope_id(in6->sin6_scope_id);
            result[ifa->ifa_name].emplace_back(address);
        }
    }

    return result;
}

[[nodiscard]] InterfaceMap get_cached_interfaces() {
    static Utils::Cache::TtlCache<std::monostate, InterfaceMap> cache(std::chrono::seconds(5));
    return cache.get_or_compute(std::monostate{}, [] { return enumerate_interfaces(); });
}
}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<std::string> ipsource::get_interfaces() {
    auto interface_map = get_cached_interfaces();
    std::vector<std::string> interfaces;
    interfaces.reserve(interface_map.size());
    std::ranges::transform(interface_map, std::back_inserter(interfaces), [](const auto& kv) { return kv.first; });
    return interfaces;
}

std::optional<std::vector<InetAddress>> ipsource::get_addresses(const std::string& interface_name) {
    auto all = get_cached_interfaces();
    if (const auto it = all.find(interface_name); it != all.end()) {
        return it->second;
    }
    return std::nullopt;
}
