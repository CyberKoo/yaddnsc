//
// Created by Kotarou on 2026/7/2.
//

#ifndef YADDNSC_UTIL_IFACE_ENUMERATOR_H
#define YADDNSC_UTIL_IFACE_ENUMERATOR_H

#include <map>
#include <string>
#include <vector>

#include "network/inet_address.h"

// ---------------------------------------------------------------------------
// Low-level enumeration of local network interfaces.
//
// Encapsulates the POSIX getifaddrs() / freeifaddrs() lifecycle with RAII,
// and converts raw sockaddr data directly into InetAddress values (skipping
// the intermediate byte-buffer step that the old code used).
//
// This layer has no caching, no mutex — pure enumeration every call.
// ---------------------------------------------------------------------------
namespace Utils::Net {

    /// Enumerate all local network interfaces.
    ///
    /// Returns a map:  interface name → list of InetAddress (IPv4 + IPv6).
    /// Only interfaces that carry at least one IPv4 or IPv6 address appear
    /// in the result.  Link-local IPv6 scope IDs are preserved.
    ///
    /// @throws std::runtime_error if getifaddrs() fails.
    [[nodiscard]] std::map<std::string, std::vector<InetAddress>> enumerate_interfaces();

} // namespace Utils::Net

#endif // YADDNSC_UTIL_IFACE_ENUMERATOR_H
