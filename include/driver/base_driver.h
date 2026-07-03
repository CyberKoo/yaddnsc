//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_BASE_DRIVER_H
#define YADDNSC_DRIVER_BASE_DRIVER_H

#include <glaze/glaze.hpp>

#include "interface/driver.h"
#include "interface/core_logger.h"
#include "interface/http_client.h"

#include "driver_ver.h"
#include "http_fmt.hpp"
#include "exceptions.h"

class BaseDriver : public Driver {
public:
    [[nodiscard]] uint32_t get_driver_version() const final {
        return DRV_VERSION;
    }

    // Default execute: generate_request -> send via HttpClient -> check_response.
    // Matches the original three-step behavior in Updater::process().
    bool execute(const DriverConfig &config, const UpdateContext &ctx, HttpClient &http) const override {
        const auto request = generate_request(config, ctx);
        CORE_LOG_DEBUG("Received DNS record update request from driver {}, {}", get_detail().name, request);

        const auto response = http.send(request);
        if (!response) {
            CORE_LOG_WARN("Update for {} failed (HTTP error: {})", ctx.fqdn, response.error());
            return false;
        }

        if (!check_response(*response)) {
            CORE_LOG_WARN("Update for {} failed: driver rejected", ctx.fqdn);
            return false;
        }

        return true;
    }

protected:
    // Parse config JSON into a typed struct with built-in validation.
    // On failure, logs the glaze error and throws MissingRequiredParamException.
    template<typename T>
    [[nodiscard]] static T parse_config(const DriverConfig &config) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, config, glz::context{});
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        throw ParamParseException(glz::format_error(ec, config));
    }
};

extern "C" Driver *create();

#endif //YADDNSC_DRIVER_BASE_DRIVER_H
