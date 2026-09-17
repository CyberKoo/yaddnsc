//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_DUCKDNS_DUCKDNS_H
#define YADDNSC_DRV_DUCKDNS_DUCKDNS_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// DuckDNS API driver for DDNS record updates.
///
/// Implements the DuckDNS HTTP API for updating A and AAAA records
/// via their simple GET-based update endpoint.
///
/// API reference: https://www.duckdns.org/spec.jsp
class DuckDnsDriver final : public yaddnsc::sdk::Driver {
public:
    ~DuckDnsDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the DuckDNS API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Build the DuckDNS API request URL from config and update params.
    static std::string generate_url(const DuckDnsParams &cfg, const yaddnsc::sdk::UpdateRequest &request);

    /// Validate the DuckDNS API response (expects "OK" or "KO").
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_DUCKDNS_DUCKDNS_H
