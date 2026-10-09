//
// ip_source — coroutine interface source.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H

#include <string>

#include "domain/network/address_family.h"
#include "infrastructure/ip_source/source.h"

namespace ipsource {

/// InterfaceIpSource — read addresses from a local network interface.
///
/// getifaddrs() is a bounded syscall with a TTL cache (InterfaceUtil), so it
/// runs directly on the loop instead of through offload; it never blocks on I/O
/// or data whose size an external peer controls (design §6.3).
class InterfaceIpSource final : public CoroIpSource {
public:
    InterfaceIpSource(std::string interface_name, domain::AddressFamily address_family);

    [[nodiscard]] coro::Task<Result> resolve() override;

private:
    std::string interface_name_;
    domain::AddressFamily address_family_;
};

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H
