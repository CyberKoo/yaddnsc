//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include "driver/base.h"

/// DNSPod API driver for DNS record updates.
///
/// Implements the DNSPod API for updating DNS records via their
/// Record.Modify endpoint.
class DNSPodDriver final : public BaseDriver {
public:
    /// Build the API request from config and update params.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Validate the DNSPod API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

private:
    /// Convert a DNSPod error code to a human-readable description.
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);
};

#endif //YADDNSC_DRV_DNSPOD_DNSPOD_H
