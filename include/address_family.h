//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_ADDRESS_FAMILY_H
#define YADDNSC_ADDRESS_FAMILY_H

/// Protocol family for IP address selection.
enum class AddressFamily {
    UNSPECIFIED, ///< No preference; use any available address
    IPV4,        ///< IPv4 only
    IPV6         ///< IPv6 only
};

#endif //YADDNSC_ADDRESS_FAMILY_H
