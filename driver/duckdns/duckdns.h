//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_DUCKDNS_DUCKDNS_H
#define YADDNSC_DRV_DUCKDNS_DUCKDNS_H

#include "driver/base.h"

/// DuckDNS API driver for DDNS record updates.
///
/// Implements the DuckDNS HTTP API for updating A and AAAA records
/// via their simple GET-based update endpoint.
///
/// API reference: https://www.duckdns.org/spec.jsp
class DuckDnsDriver final : public BaseDriver {
public:
    ~DuckDnsDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the DuckDNS API response (expects "OK" or "KO").
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;
};

#endif //YADDNSC_DRV_DUCKDNS_DUCKDNS_H
