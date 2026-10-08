//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_INTERFACE_UTIL_H
#define YADDNSC_INTERFACE_UTIL_H

#include <optional>
#include <string>
#include <vector>

class InetAddress;

/// InterfaceUtil — low-level utility for enumerating local network interfaces
///                 and their IP addresses.
///
/// Each call reads a live getifaddrs() snapshot. No shared cache, locks or
/// single-flight completion waits. A name may disappear between calls.
///
/// getifaddrs() is a kernel call that returns a bounded, small snapshot, so it
/// is an accepted blocking point on the loop thread (system metadata read).
///
/// @note Thread-safe: snapshots are owned independently by each caller.
namespace ipsource {
/// Get a list of all network interface names that have at least one
/// IPv4 or IPv6 address.
/// @return  Interface names (e.g. "eth0", "lo", "wlan0").
[[nodiscard]] std::vector<std::string> get_interfaces();

/// Get all IP addresses (v4 and v6) assigned to a given interface.
/// @param interface_name  Name of the network interface.
/// @return                IP addresses assigned to the interface;
///                        std::nullopt when the interface does not exist.
[[nodiscard]] std::optional<std::vector<InetAddress>> get_addresses(const std::string& interface_name);
}  // namespace ipsource

#endif  // YADDNSC_INTERFACE_UTIL_H
