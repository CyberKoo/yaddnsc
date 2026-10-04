//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_HTTP_IP_SOURCE_H
#define YADDNSC_HTTP_IP_SOURCE_H

#include <memory>
#include <string>

#include "domain/network/address_family.h"
#include "infrastructure/ip_source/base.h"
#include "infrastructure/network/http/types.h"

namespace net::http {
class PersistentClient;
}

namespace Utils {
class CancellationToken;
}

/// HttpIpSource — fetches the local public IP address from an external HTTP service.
///
/// Uses net::http::PersistentClient to maintain keep-alive efficiency
/// across resolve() calls: the client is bound to this source's origin at
/// construction and reuses one connection for every lookup. The address
/// family and outbound interface binding are passed through to the
/// underlying transport; cancellation flows through resolve() as a
/// parameter.
///
/// resolve() returns one address or an error (including a response-family mismatch).
class HttpIpSource final : public IpSourceBase {
public:
    /// Construct with an HTTP URL and optional filtering parameters.
    /// @param url              URL of the HTTP IP detection service.
    /// @param address_family   Address family for the connection and response IP;
    ///                         UNSPECIFIED accepts either response family.
    /// @param bind_interface   Outbound network interface to bind to (empty = any).
    /// @param base_options     Shared HTTP policy (user agent, bootstrap DNS,
    ///                         CA discovery) built once by the composition root;
    ///                         the address family / interface overrides are
    ///                         applied on top of it here.
    explicit HttpIpSource(std::string url, AddressFamily address_family = AddressFamily::UNSPECIFIED,
                          std::string bind_interface = {}, net::http::Options base_options = {});

    ~HttpIpSource() override;

    [[nodiscard]] Result resolve(const Utils::CancellationToken& token) const override;

private:
    std::string url_;
    AddressFamily address_family_;
    std::string bind_interface_;
    std::unique_ptr<net::http::PersistentClient> client_;
};

#endif  // YADDNSC_HTTP_IP_SOURCE_H
