#ifndef YADDNSC_DOMAIN_CONFIG_IP_SOURCE_KIND_H
#define YADDNSC_DOMAIN_CONFIG_IP_SOURCE_KIND_H

/// Available IP address source backends.
///
/// Lives in a dependency-free header so the domain runtime model can use the
/// enum without pulling in the Glaze-based raw config DTO. The glaze mapping
/// (including the legacy "url" alias) stays in src/infrastructure/config/parser.hpp.
namespace domain {
enum class IpSource {
    INTERFACE,  ///< Read IP from a local network interface
    HTTP,       ///< Query an external HTTP endpoint for the public IP
    MDNS        ///< Resolve via mDNS (RFC 6762, .local domain)
};
}

#endif  // YADDNSC_DOMAIN_CONFIG_IP_SOURCE_KIND_H
