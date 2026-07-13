//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_GODADDY_GODADDY_H
#define YADDNSC_DRV_GODADDY_GODADDY_H

#include "driver/base.h"

/// GoDaddy API driver for DNS record updates.
///
/// Implements the GoDaddy Domains API v1 for updating DNS records
/// via their record replacement endpoint.
///
/// API reference: https://developer.godaddy.com/doc/endpoint/domains
class GoDaddyDriver final : public BaseDriver {
public:
    ~GoDaddyDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the GoDaddy API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;
};

#endif //YADDNSC_DRV_GODADDY_GODADDY_H
