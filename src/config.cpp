#include "config.h"

#include <filesystem>

#include "config_parser.h"
#include "fmt.h"

Config::config Config::load_config(const std::string &config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("Config file {} does not exist", config_path));
    }

    config cfg{};
    if (const auto ec = glz::read_file_json(cfg, config_path, std::string{})) {
        throw std::runtime_error(fmt::format("Failed to parse config file, error: {}", glz::format_error(ec)));
    }

    return cfg;
}
