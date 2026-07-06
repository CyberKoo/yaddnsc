//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_INTERFACE_UTIL_H
#define YADDNSC_INTERFACE_UTIL_H

#include <string>
#include <vector>

class InetAddress;

/// InterfaceUtil — low-level utility for enumerating local network interfaces
///                 and their IP addresses.
///
/// Encapsulates the getifaddrs() call with a short-lived TTL cache so that
/// multiple callers (InterfaceIpSource, ConfigValidator, CLI) share the same
/// snapshot without hammering the kernel.
///
/// @note Thread-safe: all public functions are guarded by an internal mutex.
namespace InterfaceUtil {
    /// Get a list of all network interface names that have at least one
    /// IPv4 or IPv6 address.
    /// @return  Interface names (e.g. "eth0", "lo", "wlan0").
    [[nodiscard]] std::vector<std::string> get_interfaces();

    /// Get all IP addresses (v4 and v6) assigned to a given interface.
    /// @param interface_name  Name of the network interface.
    /// @return                IP addresses assigned to the interface.
    /// @throws std::runtime_error  If the interface does not exist.
    [[nodiscard]] std::vector<InetAddress> get_addresses(const std::string &interface_name);
} // namespace InterfaceUtil

#endif // YADDNSC_INTERFACE_UTIL_H
