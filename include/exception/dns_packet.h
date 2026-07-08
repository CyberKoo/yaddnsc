//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_EXCEPTION_DNS_PACKET_H
#define YADDNSC_EXCEPTION_DNS_PACKET_H

#include "base.h"

/// Thrown when a DNS wire-format packet cannot be constructed.
///
/// Indicates invalid or out-of-range input values (e.g. label > 63 octets,
/// domain name > 255 octets, EDNS version != 0).
class DnsPacketException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "DnsPacketException";
    }
};

#endif  // YADDNSC_EXCEPTION_DNS_PACKET_H
