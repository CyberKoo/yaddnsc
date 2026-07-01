//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_HTTP_IP_SOURCE_H
#define YADDNSC_HTTP_IP_SOURCE_H

#include <string>

#include "base.h"
#include "type.h"

// ---------------------------------------------------------------------------
// HttpIpSource — fetches the local IP address from an external HTTP service.
//
// Uses TransientHttpClient to perform a one-shot GET request and parses the
// response body as an IP address.  The address family and outbound interface
// binding are passed through to the underlying HTTP client.
//
// resolve() returns 0 or 1 addresses.
// ---------------------------------------------------------------------------
class HttpIpSource final : public IpSourceBase {
public:
    explicit HttpIpSource(std::string url, address_family_type af = address_family_type::UNSPECIFIED,
                          std::string bind_interface = {});

    [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
    std::string url_;
    address_family_type af_;
    std::string bind_interface_;
};

#endif // YADDNSC_HTTP_IP_SOURCE_H
