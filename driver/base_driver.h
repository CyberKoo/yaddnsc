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

#include <fmt/args.h>
#include <spdlog/spdlog.h>

#include "IDriver.h"
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

    static std::string vformat(std::string_view format, const std::vector<std::string_view> &args) {
        std::vector<fmt::basic_format_arg<fmt::format_context> > fmt_args;

        std::ranges::transform(args, std::back_inserter(fmt_args), [](std::string_view arg) {
            return fmt::detail::make_arg<fmt::format_context>(arg);
        });

        return fmt::vformat(format, fmt::basic_format_args(fmt_args.data(), static_cast<int>(fmt_args.size())));
    }

    static std::string vformat(std::string_view format, const std::map<std::string, std::string> &args) {
        fmt::dynamic_format_arg_store<fmt::format_context> store;

        for (auto const &[key, val]: args) {
            store.push_back(fmt::arg(key.c_str(), val));
        }

        return fmt::vformat(format, store);
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
