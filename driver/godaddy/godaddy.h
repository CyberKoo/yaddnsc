//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_GODADDY_GODADDY_H
#define YADDNSC_DRV_GODADDY_GODADDY_H

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// GoDaddy API driver for DNS record updates.
///
/// Implements the GoDaddy Domains API v1 for updating DNS records
/// via their record replacement endpoint.
///
/// API reference: https://developer.godaddy.com/doc/endpoint/domains
class GoDaddyDriver final : public yaddnsc::sdk::Driver {
public:
    ~GoDaddyDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the GoDaddy API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Validate the GoDaddy API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_GODADDY_GODADDY_H
