//
// Created by Kotarou on 2026/7/3.
//

#ifndef YADDNSC_DNS_TYPE_H
#define YADDNSC_DNS_TYPE_H

#include <cstdint>
#include <string>

/// Shared types used across config and DNS layers.
namespace DNS {
    /// DNS record types supported by the updater.
    enum class Type {
        A,    ///< IPv4 address record
        AAAA, ///< IPv6 address record
        TXT,  ///< Text record
        SOA   ///< Start of Authority record
    };

    /// A DNS server endpoint.
    struct Server {
        std::string address; ///< Hostname or IP address of the DNS server
        std::uint16_t port{53};   ///< UDP/TCP port (default: 53)
    };
}

#endif // YADDNSC_DNS_TYPE_H
