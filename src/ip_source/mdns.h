//
// Created by Kotarou on 2026/7/2.
//

#ifndef YADDNSC_MDNS_IP_SOURCE_H
#define YADDNSC_MDNS_IP_SOURCE_H

#include <string>

#include "base.h"
#include "dns_type.h"

// ---------------------------------------------------------------------------
// MdnsIpSource — discovers a LAN device's IP address via mDNS (RFC 6762).
//
// Sends a multicast DNS query for `hostname_` (e.g. "my-printer.local") on the
// specified network interface and returns the IP address(es) from the response.
//
// The address family is determined by `type_`:
//   • DNS::Type::A    → IPv4 multicast to 224.0.0.251:5353
//   • DNS::Type::AAAA → IPv6 multicast to ff02::fb:5353
//
// resolve() returns 0 or more InetAddress(es).  The caller (Updater) applies
// further filtering (link-local, ULA, etc.) via filter_ipv6_candidates().
//
// Thread-safe: resolve() is const and opens/closes its own socket per call.
// ---------------------------------------------------------------------------
class MdnsIpSource final : public IpSourceBase {
public:
    /// @param hostname   The mDNS hostname to query, e.g. "printer.local"
    /// @param type       DNS::Type::A for IPv4 or DNS::Type::AAAA for IPv6
    /// @param interface  Outbound network interface name (e.g. "eth0")
    MdnsIpSource(std::string hostname, DNS::Type type, std::string interface);

    [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
    std::string hostname_;
    DNS::Type type_;
    std::string interface_;
};

#endif // YADDNSC_MDNS_IP_SOURCE_H
