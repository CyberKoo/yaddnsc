//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_CONFIG_DNS_CONFIG_H
#define YADDNSC_CONFIG_DNS_CONFIG_H

#include <cstdint>
#include <string>

/// DNS-related configuration types.
namespace Config {
    /// DNS server endpoint (configuration value object).
    struct DnsServer {
        std::string address;  ///< Hostname or IP address of the DNS server
        std::uint16_t port{53};  ///< UDP/TCP port (default: 53)
    };

    /// DNS resolution strategy used by ResolverDispatcher.
    enum class ResolverStrategy {
        FALLBACK,   ///< Try resolvers sequentially until one succeeds
        CONCURRENT  ///< Query all resolvers concurrently and take the first result
    };
}

#endif // YADDNSC_CONFIG_DNS_CONFIG_H
