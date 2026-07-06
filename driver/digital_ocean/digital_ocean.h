//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
#define YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H

#include "driver/base.h"

/// DigitalOcean API driver for DNS record updates.
///
/// Implements the DigitalOcean API v2 for updating DNS records
/// via their Domain Records endpoint.
class DigitalOceanDriver final : public BaseDriver {
public:
    ~DigitalOceanDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the DigitalOcean API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const override;
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
