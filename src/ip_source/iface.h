//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_INTERFACE_IP_SOURCE_H
#define YADDNSC_INTERFACE_IP_SOURCE_H

#include <string>

#include "base.h"
#include "type.h"

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
    InterfaceIpSource(std::string interface_name, address_family_type af);

    [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
    std::string interface_name_;
    address_family_type af_;
};

#endif // YADDNSC_INTERFACE_IP_SOURCE_H
