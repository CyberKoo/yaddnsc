//
// Created by Kotarou on 2026/7/2.
//

#include "net_devices.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <memory>
#include <ranges>
#include <stdexcept>

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {
    /// RAII deleter for the getifaddrs() linked list.
    struct IfAddrDeleter {
        void operator()(ifaddrs *ptr) const noexcept {
            if (ptr) {
                freeifaddrs(ptr);
            }
        }
    };

    using IfAddrPtr = std::unique_ptr<ifaddrs, IfAddrDeleter>;

    IfAddrPtr query_ifaddrs() {
        ifaddrs *ifa = nullptr;
        if (getifaddrs(&ifa) == -1) {
            throw std::runtime_error("getifaddrs() failed");
        }
        return IfAddrPtr(ifa);
    }
} // anonymous namespace

// ===========================================================================
// NetDevices::enumerate_interfaces
// ===========================================================================

namespace NetDevices {
    std::map<std::string, std::vector<InetAddress> > enumerate_interfaces() {
        std::map<std::string, std::vector<InetAddress> > result;
        auto ifaddrs = query_ifaddrs();

        for (auto *ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) {
                continue;
            }

            const auto family = ifa->ifa_addr->sa_family;

            if (family == AF_INET) {
                const auto *in = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
                Inet4Address::addr_type arr{};
                std::ranges::copy(
                    reinterpret_cast<const uint8_t *>(&in->sin_addr.s_addr),
                    reinterpret_cast<const uint8_t *>(&in->sin_addr.s_addr) + Inet4Address::ADDR_LEN,
                    arr.begin()
                );
                result[ifa->ifa_name].emplace_back(Inet4Address::from_bytes(arr));
            } else if (family == AF_INET6) {
                const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(ifa->ifa_addr);
                Inet6Address::addr_type arr{};
                std::ranges::copy(in6->sin6_addr.s6_addr, arr.begin());
                auto v6 = Inet6Address::from_bytes(arr);
                v6.set_scope_id(in6->sin6_scope_id);
                result[ifa->ifa_name].emplace_back(v6);
            }
        }

        return result;
    }

    std::vector<Ipv4Subnet> get_ipv4_subnets(const std::string &iface_name) {
        std::vector<Ipv4Subnet> result;

        ifaddrs *raw = nullptr;
        if (getifaddrs(&raw) == -1) {
            return result;
        }
        IfAddrPtr ifaddrs(raw);

        for (auto *ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) continue;
            if (iface_name != ifa->ifa_name || ifa->ifa_addr->sa_family != AF_INET) continue;

            const auto *in = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
            const auto *mask = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask);

            Inet4Address::addr_type addr_arr{};
            std::ranges::copy(
                reinterpret_cast<const uint8_t *>(&in->sin_addr.s_addr),
                reinterpret_cast<const uint8_t *>(&in->sin_addr.s_addr) + Inet4Address::ADDR_LEN,
                addr_arr.begin()
            );

            Inet4Address::addr_type mask_arr{};
            std::ranges::copy(
                reinterpret_cast<const uint8_t *>(&mask->sin_addr.s_addr),
                reinterpret_cast<const uint8_t *>(&mask->sin_addr.s_addr) + Inet4Address::ADDR_LEN,
                mask_arr.begin()
            );

            result.push_back({Inet4Address::from_bytes(addr_arr), Inet4Address::from_bytes(mask_arr)});
        }

        return result;
    }
} // namespace NetDevices
