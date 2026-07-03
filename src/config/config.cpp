#include "config.h"
#include "parser.hpp" // IWYU pragma: keep

#include <filesystem>

#include <glaze/glaze.hpp>

#include "fmt.hpp"

Config::AppConfig Config::load_config(const std::string &config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("Config file \"{}\" does not exist", config_path));
    }

    AppConfig cfg{};
    std::string buffer;
    if (const auto ec = glz::read_file_json(cfg, config_path, buffer)) {
        throw std::runtime_error(
            fmt::format("Failed to parse config file, error: \"{}\"", glz::format_error(ec, buffer))
        );
    }

    return cfg;
}
