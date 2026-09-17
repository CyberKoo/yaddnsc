//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_PORKBUN_PORKBUN_H
#define YADDNSC_DRV_PORKBUN_PORKBUN_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// Porkbun API v3 driver for DNS record updates.
///
/// Implements the Porkbun DNS API for updating A and AAAA records
/// via their edit by name and type endpoint.
///
/// API reference: https://porkbun.com/api/json/v3/documentation
class PorkbunDriver final : public yaddnsc::sdk::Driver {
public:
    ~PorkbunDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the Porkbun API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Build the JSON request body for a Porkbun DNS record update.
    static std::string generate_body(const PorkbunParams &cfg, const yaddnsc::sdk::UpdateRequest &request);

    /// Validate the Porkbun API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_PORKBUN_PORKBUN_H
