//
// Created by Kotarou on 2026/7/3.
//

#ifndef YADDNSC_DNS_TYPE_H
#define YADDNSC_DNS_TYPE_H

#include <cstdint>
#include <string>

/// Shared types used across config and DNS layers.
/// DNS record kinds supported by the DDNS updater.
///
/// This is a subset of wire-format DNS record types (DNS::RecordType)
/// that the updater can query and update.
enum class RecordKind {
    A,    ///< IPv4 address record
    AAAA, ///< IPv6 address record
    TXT,  ///< Text record
};

/// A DNS server endpoint.
struct DnsServer {
    std::string address; ///< Hostname or IP address of the DNS server
    std::uint16_t port{53};   ///< UDP/TCP port (default: 53)
};

#endif // YADDNSC_DNS_TYPE_H
