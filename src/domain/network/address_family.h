#ifndef YADDNSC_DOMAIN_NETWORK_ADDRESS_FAMILY_H
#define YADDNSC_DOMAIN_NETWORK_ADDRESS_FAMILY_H

namespace domain {

/// Protocol family for IP address selection.
enum class AddressFamily {
    UNSPECIFIED,  ///< No preference; use any available address
    IPV4,         ///< IPv4 only
    IPV6          ///< IPv6 only
};

}  // namespace domain

#endif  // YADDNSC_DOMAIN_NETWORK_ADDRESS_FAMILY_H
