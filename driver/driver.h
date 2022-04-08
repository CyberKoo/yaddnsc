//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_H
#define YADDNSC_DRIVER_H

#include <map>
#include <string>
#include <vector>
#include <optional>

#include <fmt/args.h>
#include <spdlog/spdlog.h>

#include "IDriver.h"
#include "driver_ver.h"
#include "missing_required_param_exception.h"

class Driver : public IDriver {
public:
    void check_required_params(const driver_config_t &config) {
        for (auto &name: required_param) {
            if (config.find(name) == config.end()) {
                throw MissingRequiredParamException(fmt::format("Missing required parameter \"{}\"", "name"));
            }
        }
    }

    static std::optional<std::string> get_optional(const driver_config_t &config, std::string_view name) {
        if (config.find(name.data()) != config.end()) {
            return {config.at(name.data())};
        }

        return std::nullopt;
    }

    static std::string vformat(std::string_view format, const std::vector<std::string_view> &args) {
        std::vector<fmt::basic_format_arg<fmt::format_context>> fmt_args;

        std::transform(args.begin(), args.end(), std::back_inserter(fmt_args), [](std::string_view arg) {
            return fmt::detail::make_arg<fmt::format_context>(arg);
        });

        return fmt::vformat(format, fmt::basic_format_args<fmt::format_context>(fmt_args.data(), fmt_args.size()));
    }

    static std::string vformat(std::string_view format, const std::map<std::string, std::string> &args) {
        fmt::dynamic_format_arg_store<fmt::format_context> store;

        for (auto const &[key, val]: args) {
            store.push_back(fmt::arg(key.c_str(), val));
        }

        return fmt::vformat(format, store);
    }

    std::string_view get_driver_version() override {
        return DRV_VERSION;
    };

    void init_logger(int level, std::string_view pattern) override {
        spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
    }
};

#endif //YADDNSC_DRIVER_H
