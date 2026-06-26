#include "config.h"
#include "config_parser.hpp" // IWYU pragma: keep

#include <filesystem>

#include <glaze/glaze.hpp>

#include "fmt.hpp"

Config::config Config::load_config(const std::string &config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("Config file {} does not exist", config_path));
    }

    config cfg{};
    std::string buffer;
    if (const auto ec = glz::read_file_json(cfg, config_path, buffer)) {
        throw std::runtime_error(fmt::format("Failed to parse config file, error: {}", glz::format_error(ec, buffer)));
    }

    return cfg;
}
