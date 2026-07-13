//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_PORKBUN_PORKBUN_H
#define YADDNSC_DRV_PORKBUN_PORKBUN_H

#include "driver/base.h"

/// Porkbun API v3 driver for DNS record updates.
///
/// Implements the Porkbun DNS API for updating A and AAAA records
/// via their edit by name and type endpoint.
///
/// API reference: https://porkbun.com/api/json/v3/documentation
class PorkbunDriver final : public BaseDriver {
public:
    ~PorkbunDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the Porkbun API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

private:
    /// Build the JSON request body for a Porkbun DNS record update.
    static std::string generate_body(const DriverUpdateParams &ctx, std::optional<int> ttl);
};

#endif //YADDNSC_DRV_PORKBUN_PORKBUN_H
