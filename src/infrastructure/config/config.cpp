//
// Created by Kotarou on 2022/4/6.
//

#include "config.h"
#include "parser.hpp" // IWYU pragma: keep

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

#include <glaze/glaze.hpp>

#include "support/fmt.hpp"

namespace {
    [[nodiscard]] bool is_sensitive_key(std::string_view key) {
        std::string lower(key.size(), '\0');
        std::ranges::transform(key, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("token") != std::string::npos || lower.find("password") != std::string::npos ||
               lower.find("secret") != std::string::npos || lower.find("key") != std::string::npos;
    }

    void redact_sensitive_fields(glz::generic &value) {
        if (value.is_object()) {
            for (auto &entry: value.get_object()) {
                if (is_sensitive_key(entry.first)) {
                    entry.second = "***";
                } else {
                    redact_sensitive_fields(entry.second);
                }
            }
        } else if (value.is_array()) {
            for (auto &element: value.get_array()) {
                redact_sensitive_fields(element);
            }
        }
    }
} // namespace

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

std::string Config::redacted_json(AppConfig config) {
    for (auto &domain_config: config.domains) {
        for (auto &subdomain: domain_config.subdomains) {
            redact_sensitive_fields(subdomain.driver_param);
        }
    }

    std::string json;
    if (const auto ec = glz::write_json(config, json)) {
        throw std::runtime_error(fmt::format("Failed to serialize config: {}", glz::format_error(ec)));
    }
    return json;
}
