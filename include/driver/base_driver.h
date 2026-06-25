//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_BASE_DRIVER_H
#define YADDNSC_DRIVER_BASE_DRIVER_H

#include <glaze/glaze.hpp>

#include "interfaces/driver.h"
#include "interfaces/core_logger.h"
#include "interfaces/http_client.h"

#include "driver_ver.h"
#include "driver/exceptions.h"
#include "http_type_formatter.hpp"

class BaseDriver : public IDriver {
public:
    [[nodiscard]] uint32_t get_driver_version() const final {
        return DRV_VERSION;
    }

    // Default execute: generate_request -> send via IHttpSender -> check_response.
    // Matches the original three-step behavior in Updater::process().
    bool execute(const driver_config_type &config, const UpdateContext &ctx, IHttpSender &http) override {
        const auto req = generate_request(config, ctx);
        CORE_LOG_DEBUG("Received DNS record update request from driver {}, {}", get_detail().name, req);

        const auto resp = http.send(req);
        if (!resp.success) {
            CORE_LOG_WARN("Update for {} failed (HTTP error: {})", ctx.fqdn, resp.error_message);
            return false;
        }

        if (!check_response(resp.body)) {
            CORE_LOG_WARN("Update domain {} failed (driver rejected the response)", ctx.fqdn);
            return false;
        }

        return true;
    }

protected:
    // Parse config JSON into a typed struct with built-in validation.
    // On failure, logs the glaze error and throws MissingRequiredParamException.
    template<typename T>
    [[nodiscard]] static T parse_config(const driver_config_type &config) {
        T value{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, config, glz::context{});
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        throw ParamParseException(glz::format_error(ec, config));
    }
};

extern "C" IDriver* create();

#endif //YADDNSC_DRIVER_BASE_DRIVER_H
