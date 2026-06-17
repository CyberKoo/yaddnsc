#include "config.h"

#include <fstream>
#include <filesystem>

#include "fmt.h"

Config::config Config::load_config(std::string_view config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("config {} not exists", config_path));
    }

    std::ifstream fin(config_path.data());
    if (!fin) {
        throw std::runtime_error("failed to read config file");
    }

    std::string content((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

    config cfg{};
    auto ec = glz::read_json(cfg, content);
    if (ec) {
        throw std::runtime_error(fmt::format("failed to parse config file, error: {}", glz::format_error(ec, content)));
    }

    return cfg;
}
