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

private:
    /// Validate the DigitalOcean API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
