//
// ip_source — coroutine HTTP source.
//

#ifndef YADDNSC_IP_SOURCE_CORO_HTTP_H
#define YADDNSC_IP_SOURCE_CORO_HTTP_H

#include <string>

#include "domain/network/address_family.h"
#include "infrastructure/ip_source/coro/source.h"
#include "infrastructure/net/http/types.h"

namespace ipsource {

/// HttpIpSource — fetch the local public address from an HTTP service.
///
/// Built on the coroutine http::Client; the address-family and interface
/// overrides are applied on top of the composition root's shared HTTP policy.
class HttpIpSource final : public CoroIpSource {
public:
    /// @param url             HTTP IP-detection endpoint.
    /// @param address_family  Expected response family (UNSPECIFIED accepts any).
    /// @param bind_interface  Outbound interface (empty = any).
    /// @param base_options    Shared HTTP policy (user agent, bootstrap DNS, CA).
    HttpIpSource(std::string url, AddressFamily address_family, std::string bind_interface,
                 http::Options base_options);

    [[nodiscard]] coro::Task<Result> resolve() override;

private:
    std::string url_;
    AddressFamily address_family_;
    http::Options options_;
};

}  // namespace ipsource

#endif  // YADDNSC_IP_SOURCE_CORO_HTTP_H
