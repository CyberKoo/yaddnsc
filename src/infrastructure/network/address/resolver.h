#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_ADDRESS_RESOLVER_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_ADDRESS_RESOLVER_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <expected>

#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"

namespace net {

/// Injected hostname lookup. The callable owns its policy and returns a task
/// owning its inputs; cancellation propagates as coro::Cancelled. Loop-thread
/// only. An empty callable means hostname resolution is unavailable.
using HostResolver = std::function<coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>>(
    std::string, std::optional<domain::AddressFamily>)>;

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_ADDRESS_RESOLVER_H
