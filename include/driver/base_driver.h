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
#include "core_logger.h"

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

    static std::optional<std::string> get_optional(const driver_config_type &config, const std::string &name) {
        if (const auto it = config.find(name); it != config.end()) {
            return {it->second};
        }

        return std::nullopt;
    }

    [[nodiscard]] std::string_view get_driver_version() const final {
        return DRV_VERSION;
    }
};

#endif //YADDNSC_BASE_DRIVER_H
