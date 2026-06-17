//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_BASE_DRIVER_H
#define YADDNSC_BASE_DRIVER_H

#include <map>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

#include "fmt.h"
#include <spdlog/spdlog.h>

#include "driver_interface.h"
#include "driver_ver.h"
#include "missing_required_param_exception.h"

class BaseDriver : public IDriver {
public:
    void check_required_params(const driver_config_type &config) const {
        for (auto &name: required_param_) {
            if (!config.contains(name)) {
                throw MissingRequiredParamException(fmt::format("Missing required parameter \"{}\"", name));
            }
        }
    }

    static std::optional<std::string> get_optional(const driver_config_type &config, std::string_view name) {
        if (config.contains(name.data())) {
            return {config.at(name.data())};
        }

        return std::nullopt;
    }

    [[nodiscard]] std::string_view get_driver_version() const final {
        return DRV_VERSION;
    }

    void init_logger(int level, std::string_view pattern) final {
        spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
        spdlog::set_pattern(pattern.data());
    }
};

#endif //YADDNSC_BASE_DRIVER_H
