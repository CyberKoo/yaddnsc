//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include <string_view>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// DNSPod API driver for DNS record updates.
///
/// Implements the DNSPod API for updating DNS records via their
/// Record.Modify endpoint.
class DNSPodDriver final : public yaddnsc::sdk::Driver {
public:
    ~DNSPodDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the DNSPod API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Build the DNSPod API request from parsed config and update params.
    [[nodiscard]] static yaddnsc::sdk::HttpRequest generate_request(const DNSPodParams &cfg,
                                                                    const yaddnsc::sdk::UpdateRequest &request);

    /// Validate the DNSPod API response.
    [[nodiscard]] static bool check_response(const yaddnsc::sdk::HttpResponse &response,
                                             const yaddnsc::sdk::Services &services);

    /// Convert a DNSPod error code to a human-readable description.
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);
};

#endif //YADDNSC_DRV_DNSPOD_DNSPOD_H
