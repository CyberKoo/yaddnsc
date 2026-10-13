#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_UTIL_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_UTIL_H

#include <optional>
#include <string>
#include <vector>

namespace domain {
class InetAddress;
enum class AddressFamily;
}  // namespace domain

/// InterfaceUtil — low-level utility for enumerating local network interfaces
///                 and their IP addresses.
///
/// Each call reads a live getifaddrs() snapshot. No shared cache, locks or
/// single-flight completion waits. A name may disappear between calls; the
/// one-pass get_all() is the consistent read for listings.
///
/// These synchronous metadata reads may block. Call them before the loop
/// starts, or in an owned coro::offload packet during runtime; never directly
/// on the loop thread. Worker inputs and snapshots must own their storage.
///
/// @note Thread-safe: snapshots are owned independently by each caller.
namespace ipsource {

/// One interface's name and addresses from a shared enumeration pass.
struct InterfaceAddresses {
    std::string name;
    std::vector<domain::InetAddress> addresses;
};

/// Get a list of all network interface names that have at least one
/// IPv4 or IPv6 address.
/// @return  Interface names (e.g. "eth0", "lo", "wlan0").
[[nodiscard]] std::vector<std::string> get_interfaces();

/// Get all IP addresses (v4 and v6) assigned to a given interface.
/// @param interface_name  Name of the network interface.
/// @return                IP addresses assigned to the interface;
///                        std::nullopt when the interface does not exist.
[[nodiscard]] std::optional<std::vector<domain::InetAddress>> get_addresses(const std::string& interface_name);

/// Every interface with its addresses from a single getifaddrs() pass —
/// the consistent one-pass form of get_interfaces() + get_addresses().
[[nodiscard]] std::vector<InterfaceAddresses> get_all();

/// Get the index of the default interface for a family: the first interface
/// that is UP, neither loopback nor point-to-point, and carries an address of
/// that family. std::nullopt when none qualifies.
[[nodiscard]] std::optional<unsigned int> get_default_interface_index(domain::AddressFamily family);
}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_UTIL_H
