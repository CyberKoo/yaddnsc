#ifndef YADDNSC_INFRASTRUCTURE_DNS_DNS_PACKET_EXCEPTION_H
#define YADDNSC_INFRASTRUCTURE_DNS_DNS_PACKET_EXCEPTION_H

#include "support/exception.h"

/// Thrown when a DNS wire-format packet cannot be constructed.
///
/// Indicates invalid or out-of-range input values (e.g. label > 63 octets,
/// domain name > 255 octets, EDNS version != 0).
class DnsPacketException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override { return "DnsPacketException"; }
};

#endif  // YADDNSC_INFRASTRUCTURE_DNS_DNS_PACKET_EXCEPTION_H
