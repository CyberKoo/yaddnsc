//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
#define YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H

#include "config.hpp"
#include "driver/base.h"

/// Cloudflare API driver for DNS record updates.
///
/// Implements the Cloudflare API v4 for updating A, AAAA, and TXT records
/// via their DNS Records endpoint.
class CloudflareDriver final : public BaseDriver {
public:
    ~CloudflareDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the Cloudflare API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const override;

private:
    /// Build the JSON request body for a Cloudflare DNS record update.
    static std::string generate_body(const CloudflareParams &cfg, const DriverUpdateParams &ctx);
};

#endif //YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
