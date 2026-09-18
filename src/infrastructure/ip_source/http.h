//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_HTTP_IP_SOURCE_H
#define YADDNSC_HTTP_IP_SOURCE_H

#include <memory>
#include <string>

#include "domain/network/address_family.h"
#include "support/util/cancellation_token.hpp"
#include "infrastructure/ip_source/base.h"

namespace net::http {
class PersistentClient;
}

/// HttpIpSource — fetches the local public IP address from an external HTTP service.
///
/// Uses net::http::Client to maintain keep-alive efficiency across
/// resolve() calls. The address family and outbound interface binding are
/// passed through to the underlying transport; cancellation is bound at
/// construction.
///
/// resolve() returns 0 or 1 addresses.
class HttpIpSource final : public IpSourceBase {
public:
    /// Construct with an HTTP URL and optional filtering parameters.
    /// @param url              URL of the HTTP IP detection service.
    /// @param address_family   Preferred address family for the connection.
    /// @param bind_interface   Outbound network interface to bind to (empty = any).
    /// @param token            Cancellation token for the underlying HTTP client.
    explicit HttpIpSource(std::string url,
                          AddressFamily address_family = AddressFamily::UNSPECIFIED,
                          std::string bind_interface = {},
                          Utils::CancellationToken token = {});

    ~HttpIpSource() override;

    [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
    std::string url_;
    AddressFamily address_family_;
    std::string bind_interface_;
    std::unique_ptr<net::http::PersistentClient> client_;
};

#endif  // YADDNSC_HTTP_IP_SOURCE_H
