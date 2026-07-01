//
// Created by Kotarou on 2026/7/1.
//

#include "iface_util.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <map>
#include <mutex>
#include <chrono>
#include <ranges>
#include <algorithm>
#include <stdexcept>
#include <functional>

#include "fmt.hpp"
#include "network/inet_address.h"

namespace {

    // ── Internal types ──────────────────────────────────────────────────
    struct interface_address {
        std::vector<uint8_t> addr_bytes;
    };

    using ifaddr_ptr = std::unique_ptr<ifaddrs, std::function<void(ifaddrs *)> >;

    // ── Cached interface map ────────────────────────────────────────────

    struct InterfaceCache {
        std::mutex mtx;
        std::map<std::string, std::vector<interface_address> > data;
        std::chrono::steady_clock::time_point last_refresh;
        bool populated = false;
        static constexpr auto TTL = std::chrono::seconds(5);
    };

    InterfaceCache &cache() {
        static InterfaceCache c;
        return c;
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    ifaddr_ptr get_ifaddrs() {
        ifaddrs *ifa;
        if (getifaddrs(&ifa) == -1) {
            throw std::runtime_error("getifaddrs() failed");
        }
        return {ifa, [](ifaddrs *a) { freeifaddrs(a); }};
    }

    std::map<std::string, std::vector<interface_address> > collect_interface_addresses() {
        std::map<std::string, std::vector<interface_address> > result;
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

            result[ifa->ifa_name].emplace_back(interface_address{std::move(bytes)});
        }

        return result;
    }

    const std::map<std::string, std::vector<interface_address> > &get_all_interface_addresses() {
        auto &c = cache();
        std::lock_guard lock(c.mtx);
        const auto now = std::chrono::steady_clock::now();
        if (!c.populated || now - c.last_refresh >= InterfaceCache::TTL) {
            c.data = collect_interface_addresses();
            c.last_refresh = now;
            c.populated = true;
        }
        return c.data;
    }

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<std::string> InterfaceUtil::get_interfaces() {
    const auto &interface_map = get_all_interface_addresses();
    std::vector<std::string> interfaces;
    interfaces.reserve(interface_map.size());
    std::ranges::transform(interface_map, std::back_inserter(interfaces),
                           [](const auto &kv) { return kv.first; });
    return interfaces;
}

std::vector<InetAddress>
InterfaceUtil::get_addresses(const std::string &interface_name) {
    const auto &all = get_all_interface_addresses();
    if (auto it = all.find(interface_name); it != all.end()) {
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
