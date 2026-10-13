//
// ip_source — coroutine interface source.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_IFACE_H

#include <string>

#include "coro/task.hpp"
#include "infrastructure/ip_source/result.h"

namespace domain {
enum class AddressFamily;
}  // namespace domain

namespace ipsource {

/// InterfaceIpSource — read addresses from a local network interface.
///
/// Each lookup reads a live getifaddrs() snapshot without a shared cache.
/// Metadata reads run in owned offload packets, so cancellation can release
/// the caller while the worker finishes without borrowing this source.
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
