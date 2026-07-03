//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_INTERFACE_IP_SOURCE_H
#define YADDNSC_INTERFACE_IP_SOURCE_H

#include <string>

#include "base.h"
#include "address_family.h"

// ---------------------------------------------------------------------------
// InterfaceIpSource — reads IP addresses from a local network interface.
//
// Filters the results by address family only (link-local / ULA filtering is
// the caller's responsibility).  Returns all matching addresses so the caller
// can apply policy filters.
//
// Thread-safe: the underlying InterfaceUtil uses a mutex-guarded cache.
// ---------------------------------------------------------------------------
class InterfaceIpSource final : public IpSourceBase {
public:
    InterfaceIpSource(std::string interface_name, AddressFamily address_family);

    [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
    std::string interface_name_;
    AddressFamily address_family_;
};

#endif // YADDNSC_INTERFACE_IP_SOURCE_H
