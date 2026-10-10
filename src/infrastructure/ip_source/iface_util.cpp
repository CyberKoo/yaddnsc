#include "iface_util.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "domain/network/inet_address.h"
#include "domain/network/address_family.h"

// ===========================================================================
// Internal enumeration
// ===========================================================================

namespace {
using InterfaceMap = std::map<std::string, std::vector<domain::InetAddress>>;

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
            domain::Inet4Address::AddrType array{};
            const auto bytes =
                std::span{reinterpret_cast<const std::uint8_t*>(&in->sin_addr.s_addr), domain::Inet4Address::ADDR_LEN};
            std::ranges::copy(bytes, array.begin());
            result[ifa->ifa_name].emplace_back(domain::Inet4Address::from_bytes(array));
        } else if (family == AF_INET6) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
            domain::Inet6Address::AddrType array{};
            std::ranges::copy(in6->sin6_addr.s6_addr, array.begin());
            auto address = domain::Inet6Address::from_bytes(array);
            address.set_scope_id(in6->sin6_scope_id);
            result[ifa->ifa_name].emplace_back(address);
        }
    }

    return result;
}
}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<std::string> ipsource::get_interfaces() {
    auto interface_map = enumerate_interfaces();
    std::vector<std::string> interfaces;
    interfaces.reserve(interface_map.size());
    std::ranges::transform(interface_map, std::back_inserter(interfaces), [](const auto& kv) { return kv.first; });
    return interfaces;
}

std::optional<std::vector<domain::InetAddress>> ipsource::get_addresses(const std::string& interface_name) {
    auto all = enumerate_interfaces();
    if (const auto it = all.find(interface_name); it != all.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<unsigned int> ipsource::get_default_interface_index(const domain::AddressFamily family) {
    const auto native = family == domain::AddressFamily::IPV6 ? AF_INET6 : AF_INET;
    auto ifaddrs = query_ifaddrs();
    for (auto* ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || static_cast<int>(ifa->ifa_addr->sa_family) != native) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0 ||
            (ifa->ifa_flags & IFF_POINTOPOINT) != 0) {
            continue;
        }
        if (const unsigned int index = ::if_nametoindex(ifa->ifa_name); index > 0) {
            return index;
        }
    }
    return std::nullopt;
}
