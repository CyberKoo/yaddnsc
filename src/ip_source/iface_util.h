//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_INTERFACE_UTIL_H
#define YADDNSC_INTERFACE_UTIL_H

#include <string>
#include <vector>

class InetAddress;

// ---------------------------------------------------------------------------
// InterfaceUtil — low-level utility for enumerating local network interfaces
//                 and their IP addresses.
//
// Encapsulates the getifaddrs() call with a short-lived TTL cache so that
// multiple callers (InterfaceIpSource, ConfigValidator, CLI) share the same
// snapshot without hammering the kernel.
//
// Thread-safe: all public functions are guarded by an internal mutex.
// ---------------------------------------------------------------------------
namespace InterfaceUtil {
    // Returns a list of all network interface names that have at least one
    // IPv4 or IPv6 address.
    [[nodiscard]] std::vector<std::string> get_interfaces();

    // Returns all IP addresses (v4 and v6) assigned to a given interface.
    // Throws std::runtime_error if the interface does not exist.
    [[nodiscard]] std::vector<InetAddress> get_addresses(const std::string &interface_name);
} // namespace InterfaceUtil

#endif // YADDNSC_INTERFACE_UTIL_H
