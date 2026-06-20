//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_BASE_DRIVER_H
#define YADDNSC_DRIVER_BASE_DRIVER_H

#include <glaze/glaze.hpp>

#include "driver_ver.h"
#include "driver_interface.h"
#include "driver/exceptions.h"

class BaseDriver : public IDriver {
public:
    [[nodiscard]] uint32_t get_driver_version() const final {
        return DRV_VERSION;
    }

protected:
    // Parse config JSON into a typed struct with built-in validation.
    // On failure, logs the glaze error and throws MissingRequiredParamException.
    template<typename T>
    [[nodiscard]] static T parse_config(const driver_config_type &config) {
        T value{};
        glz::context ctx{};
        const auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(value, config, ctx);
        if (ec == glz::error_code::none) [[likely]] {
            return value;
        }

        throw ParamParseException(glz::format_error(ec, config));
    }
};

extern "C" IDriver* create();

#endif //YADDNSC_DRIVER_BASE_DRIVER_H
