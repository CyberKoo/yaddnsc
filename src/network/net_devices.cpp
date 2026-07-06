//
// Created by Kotarou on 2026/7/2.
//

#include "net_devices.h"

#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <span>
#include <ranges>
#include <memory>
#include <stdexcept>
#include <cstdint>

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {
    /// RAII deleter for the getifaddrs() linked list.
    using IfAddrPtr = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>;

    [[nodiscard]] IfAddrPtr query_ifaddrs() {
        ifaddrs *ifa = nullptr;
        if (getifaddrs(&ifa) == -1) {
            throw std::runtime_error("getifaddrs() failed");
        }

        return {ifa, &freeifaddrs};
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
                auto bytes = std::span{
                    reinterpret_cast<const std::uint8_t *>(&in->sin_addr.s_addr),
                    Inet4Address::ADDR_LEN
                };
                std::ranges::copy(bytes, arr.begin());
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
        auto ifaddrs = query_ifaddrs();

        for (auto *ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) continue;
            if (iface_name != ifa->ifa_name || ifa->ifa_addr->sa_family != AF_INET) continue;

            const auto *in = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
            const auto *mask = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask);

            Inet4Address::addr_type addr_arr{};
            auto addr_bytes = std::span{
                reinterpret_cast<const std::uint8_t *>(&in->sin_addr.s_addr), Inet4Address::ADDR_LEN
            };
            std::ranges::copy(addr_bytes, addr_arr.begin());

            Inet4Address::addr_type mask_arr{};
            auto mask_bytes = std::span{
                reinterpret_cast<const std::uint8_t *>(&mask->sin_addr.s_addr), Inet4Address::ADDR_LEN
            };
            std::ranges::copy(mask_bytes, mask_arr.begin());

            result.push_back({Inet4Address::from_bytes(addr_arr), Inet4Address::from_bytes(mask_arr)});
        }

        return result;
    }

    unsigned int find_default_interface_index(int address_family) {
        auto ifaddrs = query_ifaddrs();

        for (auto *ifa = ifaddrs.get(); ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) {
                continue;
            }
            if (address_family != AF_UNSPEC && ifa->ifa_addr->sa_family != address_family) {
                continue;
            }
            if ((ifa->ifa_flags & IFF_UP) == 0 ||
                (ifa->ifa_flags & IFF_LOOPBACK) != 0 ||
                (ifa->ifa_flags & IFF_POINTOPOINT) != 0) {
                continue;
            }
            auto index = if_nametoindex(ifa->ifa_name);
            if (index > 0) {
                return index;
            }
        }

        return 0;
    }

    unsigned int name_to_index(const std::string &name) {
        return ::if_nametoindex(name.c_str());
    }

    std::string index_to_name(unsigned int index) {
        char buf[IF_NAMESIZE]{};
        return ::if_indextoname(index, buf) ? buf : std::string{};
    }
} // namespace NetDevices
