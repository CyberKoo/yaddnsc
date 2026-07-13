//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_LINODE_LINODE_H
#define YADDNSC_DRV_LINODE_LINODE_H

#include "driver/base.h"

/// Linode API v4 driver for DNS record updates.
///
/// Implements the Linode DNS API for updating A and AAAA records
/// via their domain records endpoint.
///
/// API reference: https://techdocs.akamai.com/linode-api/reference/put-domain-record
class LinodeDriver final : public BaseDriver {
public:
    ~LinodeDriver() override = default;

    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the Linode API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

private:
    /// Build the JSON request body for a Linode DNS record update.
    static std::string generate_body(const DriverUpdateParams &ctx, std::optional<int> ttl_sec);
};

#endif //YADDNSC_DRV_LINODE_LINODE_H
