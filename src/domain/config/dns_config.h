#ifndef YADDNSC_DOMAIN_CONFIG_DNS_CONFIG_H
#define YADDNSC_DOMAIN_CONFIG_DNS_CONFIG_H

#include <cstdint>
#include <string>

/// DNS-related configuration types.
namespace domain {
/// DNS server endpoint (configuration value object).
struct DnsServer {
    std::string address{};   ///< Hostname or IP address of the DNS server
    std::uint16_t port{53};  ///< UDP/TCP port (default: 53)
};

/// DNS resolution strategy used by ResolverDispatcher.
enum class ResolverStrategy {
    FALLBACK,    ///< Try resolvers in configured order until one succeeds
    CONCURRENT,  ///< Query resolvers concurrently and take the first result
    SHUFFLE      ///< Try resolvers sequentially in random order until one succeeds
};
}  // namespace domain

#endif  // YADDNSC_DOMAIN_CONFIG_DNS_CONFIG_H
