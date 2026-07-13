//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_VULTR_VULTR_H
#define YADDNSC_DRV_VULTR_VULTR_H

#include "driver/base.h"

/// Vultr API v2 driver for DNS record updates.
///
/// Implements the Vultr DNS API for updating A and AAAA records
/// via their domain records endpoint.
///
/// API reference: https://www.vultr.com/api/#tag/dns
class VultrDriver final : public BaseDriver {
public:
    ~VultrDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the Vultr API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

private:
    /// Build the JSON request body for a Vultr DNS record update.
    static std::string generate_body(const DriverUpdateParams &ctx, std::optional<int> ttl);
};

#endif //YADDNSC_DRV_VULTR_VULTR_H
