//
// Created by Kotarou on 2026/7/2.
//

#ifndef YADDNSC_NETWORK_NET_DEVICES_H
#define YADDNSC_NETWORK_NET_DEVICES_H

#include <sys/socket.h>

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
    [[nodiscard]] std::map<std::string, std::vector<InetAddress> > enumerate_interfaces();

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

    // -----------------------------------------------------------------------
    //  Default interface discovery
    // -----------------------------------------------------------------------

    /// Find the first non-loopback, non-tunnel, up interface whose address
    /// matches the given address family and return its numeric index.
    /// Pass AF_UNSPEC to accept any address family.
    /// Skips loopback (IFF_LOOPBACK) and point-to-point (IFF_POINTOPOINT)
    /// interfaces so that "<default>" picks a physical (or bridged) NIC.
    /// Returns 0 if no suitable interface is found — never throws.
    [[nodiscard]] unsigned int find_default_interface_index(int address_family = AF_UNSPEC);

    // -----------------------------------------------------------------------
    //  Interface name / index conversion
    // -----------------------------------------------------------------------

    /// Convert interface name to numeric index (wrapper around if_nametoindex).
    /// Returns 0 if the name does not exist.
    [[nodiscard]] unsigned int name_to_index(const std::string &name) noexcept;

    /// Convert numeric index to interface name (wrapper around if_indextoname).
    /// Returns an empty string if the index does not correspond to any interface.
    [[nodiscard]] std::string index_to_name(unsigned int index);
} // namespace NetDevices

#endif  // YADDNSC_NETWORK_NET_DEVICES_H
