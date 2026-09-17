//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
#define YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// DigitalOcean API driver for DNS record updates.
///
/// Implements the DigitalOcean API v2 for updating DNS records
/// via their Domain Records endpoint.
class DigitalOceanDriver final : public yaddnsc::sdk::Driver {
public:
    ~DigitalOceanDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the DigitalOcean API schema without
    /// updating; schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Validate the DigitalOcean API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
