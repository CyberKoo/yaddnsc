//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_HTTP_IP_SOURCE_H
#define YADDNSC_HTTP_IP_SOURCE_H

#include <memory>
#include <string>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/network/address_family.h"
#include "infrastructure/ip_source/base.h"

namespace net::http {
class PersistentClient;
}

namespace Utils {
class CancellationToken;
}

/// HttpIpSource — fetches the local public IP address from an external HTTP service.
///
/// Uses net::http::Client to maintain keep-alive efficiency across
/// resolve() calls. The address family and outbound interface binding are
/// passed through to the underlying transport; cancellation flows through
/// resolve() as a parameter.
///
/// resolve() returns 0 or 1 addresses.
class HttpIpSource final : public IpSourceBase {
public:
    /// Construct with an HTTP URL and optional filtering parameters.
    /// @param url              URL of the HTTP IP detection service.
    /// @param address_family   Preferred address family for the connection.
    /// @param bind_interface   Outbound network interface to bind to (empty = any).
    /// @param bootstrap        Bootstrap DNS servers used to resolve the URL's
    ///                         hostname (empty: hostname URLs fail fast).
    explicit HttpIpSource(std::string url,
                          AddressFamily address_family = AddressFamily::UNSPECIFIED,
                          std::string bind_interface = {},
                          std::vector<Config::DnsServer> bootstrap = {});

    ~HttpIpSource() override;

    [[nodiscard]] Result resolve(const Utils::CancellationToken& token) const override;

private:
    std::string url_;
    AddressFamily address_family_;
    std::string bind_interface_;
    std::unique_ptr<net::http::PersistentClient> client_;
};

#endif  // YADDNSC_HTTP_IP_SOURCE_H
