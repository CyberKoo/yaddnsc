//
// Created by Kotarou on 2022/4/6.
//

#include "config.h"
#include "parser.hpp" // IWYU pragma: keep

#include <filesystem>

#include <glaze/glaze.hpp>

#include "fmt.hpp"

// ===========================================================================
// Config::load_config — read and parse the JSON configuration file.
// ===========================================================================

Config::AppConfig Config::load_config(const std::string &config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("Config file \"{}\" does not exist", config_path));
    }

    AppConfig cfg{};
    std::string buffer;
    if (const auto ec = glz::read_file_json(cfg, config_path, buffer)) {
        // Do not include the buffer contents in the error: the config file
        // holds API credentials, and the message may end up in logs.
        throw std::runtime_error(
            fmt::format("Failed to parse config file \"{}\", error: \"{}\"", config_path, glz::format_error(ec))
        );
    }

    return cfg;
}
