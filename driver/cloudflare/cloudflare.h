//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
#define YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// Cloudflare API driver for DNS record updates.
///
/// Implements the Cloudflare API v4 for updating A, AAAA, and TXT records
/// via their DNS Records endpoint.
class CloudflareDriver final : public yaddnsc::sdk::Driver {
public:
    ~CloudflareDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

private:
    /// Build the JSON request body for a Cloudflare DNS record update.
    static std::string generate_body(const CloudflareParams &cfg, const yaddnsc::sdk::UpdateRequest &request);

    /// Validate the Cloudflare API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
