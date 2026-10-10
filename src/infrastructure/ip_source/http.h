//
// ip_source — coroutine HTTP source.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_HTTP_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_HTTP_H

#include <string>

#include "coro/task.hpp"
#include "infrastructure/http/types.h"
#include "infrastructure/ip_source/source.h"

namespace domain {
enum class AddressFamily;
}  // namespace domain

namespace ipsource {

/// HttpIpSource — fetch the local public address from an HTTP service.
///
/// Built on the coroutine http::Client; the address-family and interface
/// overrides are applied on top of the composition root's shared HTTP policy.
/// Cancellation: awaits are scope checkpoints and propagate coro::Cancelled.
/// Thread safety: loop thread only.
class HttpIpSource final {
public:
    /// @param url             HTTP IP-detection endpoint.
    /// @param address_family  Expected response family (UNSPECIFIED accepts any).
    /// @param bind_interface  Outbound interface (empty = any).
    /// @param base_options    Shared HTTP policy (user agent, resolver, CA).
    HttpIpSource(std::string url, domain::AddressFamily address_family, std::string bind_interface,
                 http::Options base_options);

    [[nodiscard]] coro::Task<Result> resolve();

private:
    std::string url_;
    domain::AddressFamily address_family_;
    http::Options options_;
};

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_HTTP_H
