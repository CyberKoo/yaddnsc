//
// ip_source — coroutine interface source.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H

#include <string>

#include "coro/task.hpp"
#include "infrastructure/ip_source/source.h"

namespace domain {
enum class AddressFamily;
}  // namespace domain

namespace ipsource {

/// InterfaceIpSource — read addresses from a local network interface.
///
/// Each lookup reads a live getifaddrs() snapshot without a shared cache.
/// This bounded kernel metadata read runs directly on the loop, not offload;
/// its size is not controlled by an external peer.
/// Thread safety: loop thread only.
class InterfaceIpSource final {
public:
    InterfaceIpSource(std::string interface_name, domain::AddressFamily address_family);

    [[nodiscard]] coro::Task<Result> resolve();

private:
    std::string interface_name_;
    domain::AddressFamily address_family_;
};

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H
