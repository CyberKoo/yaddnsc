//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_LINODE_LINODE_H
#define YADDNSC_DRV_LINODE_LINODE_H

#include <optional>
#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// Linode API v4 driver for DNS record updates.
///
/// Implements the Linode DNS API for updating A and AAAA records
/// via their domain records endpoint.
///
/// API reference: https://techdocs.akamai.com/linode-api/reference/put-domain-record
class LinodeDriver final : public yaddnsc::sdk::Driver {
public:
    ~LinodeDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the Linode API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Build the JSON request body for a Linode DNS record update.
    static std::string generate_body(const yaddnsc::sdk::UpdateRequest &request, std::optional<int> ttl_sec);

    /// Validate the Linode API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_LINODE_LINODE_H
