//
// Created by Kotarou on 2026/7/2.
//

#ifndef YADDNSC_NETWORK_NET_DEVICES_H
#define YADDNSC_NETWORK_NET_DEVICES_H

#include <map>
#include <string>
#include <vector>

#include "inet_address.h"

// ---------------------------------------------------------------------------
// Low-level enumeration of local network interfaces.
//
// Encapsulates the POSIX getifaddrs() / freeifaddrs() lifecycle with RAII,
// and converts raw sockaddr data directly into InetAddress values (skipping
// the intermediate byte-buffer step that the old code used).
//
// This layer has no caching, no mutex — pure enumeration every call.
// ---------------------------------------------------------------------------
namespace NetDevices {

    /// Enumerate all local network interfaces.
    ///
    /// Returns a map:  interface name → list of InetAddress (IPv4 + IPv6).
    /// Only interfaces that carry at least one IPv4 or IPv6 address appear
    /// in the result.  Link-local IPv6 scope IDs are preserved.
    ///
    /// @throws std::runtime_error if getifaddrs() fails.
    [[nodiscard]] std::map<std::string, std::vector<InetAddress>> enumerate_interfaces();

    // -----------------------------------------------------------------------
    //  IPv4 subnet query (address + netmask)
    // -----------------------------------------------------------------------

    /// IPv4 subnet information — the interface address and its netmask.
    struct Ipv4Subnet {
        Inet4Address address;
        Inet4Address netmask;
    };

    /// Return all IPv4 address/netmask pairs for a given interface.
    /// Returns an empty vector if the interface does not exist or has no IPv4
    /// addresses.  Never throws.
    [[nodiscard]] std::vector<Ipv4Subnet> get_ipv4_subnets(const std::string &iface_name);

} // namespace NetDevices

#endif // YADDNSC_NETWORK_NET_DEVICES_H
