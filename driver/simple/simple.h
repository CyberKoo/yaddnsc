//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_SIMPLE_SIMPLE_H
#define YADDNSC_DRV_SIMPLE_SIMPLE_H

#include "driver/base.h"

/// Simple HTTP GET/POST driver for DNS record updates.
///
/// The simplest driver implementation — it embeds the IP address into
/// the URL or request body and doesn't parse the response beyond checking
/// the HTTP status code.
class SimpleDriver final : public BaseDriver {
public:
    ~SimpleDriver() override = default;

    /// Build a simple HTTP request with the IP address embedded.
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

    /// Validate the response — returns true for 2xx status codes.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;
};

#endif //YADDNSC_DRV_SIMPLE_SIMPLE_H
