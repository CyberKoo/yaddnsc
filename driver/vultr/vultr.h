//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_VULTR_VULTR_H
#define YADDNSC_DRV_VULTR_VULTR_H

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// Vultr API v2 driver for DNS record updates.
///
/// Implements the Vultr DNS API for updating A and AAAA records
/// via their domain records endpoint.
///
/// API reference: https://www.vultr.com/api/#tag/dns
class VultrDriver final : public yaddnsc::sdk::Driver {
public:
    ~VultrDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

private:
    /// Validate the Vultr API response.
    [[nodiscard]] static bool check_response(const yaddnsc::sdk::HttpResponse &response,
                                             const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_VULTR_VULTR_H
