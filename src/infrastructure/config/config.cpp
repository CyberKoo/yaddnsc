#include "config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include <glaze/glaze.hpp>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/config/config_exception.h"
#include "infrastructure/config/diagnostics/parse_diagnostic.h"
#include "support/fmt.hpp"
#include "support/redact.hpp"

#include "glaze_meta.hpp"  // IWYU pragma: keep

namespace {
using Utils::redact_uri_credentials;

[[nodiscard]] bool is_sensitive_key(std::string_view key) {
    std::string lower(key.size(), '\0');
    std::ranges::transform(key, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("token") != std::string::npos || lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos || lower.find("key") != std::string::npos;
}

void redact_sensitive_fields(glz::generic& value) {
    if (value.is_object()) {
        for (auto& entry : value.get_object()) {
            if (is_sensitive_key(entry.first)) {
                entry.second = "***";
            } else {
                redact_sensitive_fields(entry.second);
            }
        }
    } else if (value.is_array()) {
        for (auto& element : value.get_array()) {
            redact_sensitive_fields(element);
        }
    }
}
}  // namespace

// ===========================================================================
// Config::load_config — read and parse the JSON configuration file.
// ===========================================================================

Config::AppConfig Config::load_config(const std::string& config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw ConfigException(fmt::format("Config file \"{}\" does not exist", config_path));
    }

    AppConfig cfg{};
    std::string buffer;
    if (const auto ec = glz::read_file_json(cfg, config_path, buffer)) {
        // The message names the position, the key, and what that key accepts,
        // but never a configuration value: the file holds API credentials and
        // the message is logged as a fatal error.
        throw ConfigException(Config::Diagnostic::describe_parse_error(config_path, buffer, ec.count, ec.ec));
    }

    return cfg;
}

std::string Config::redacted_json(AppConfig config) {
    for (auto& server : config.resolver.servers) {
        redact_uri_credentials(server.address);
    }
    for (auto& domain_config : config.domains) {
        for (auto& subdomain : domain_config.subdomains) {
            redact_uri_credentials(subdomain.ip_source_param);
            redact_sensitive_fields(subdomain.driver_params);
        }
    }

    std::string json;
    if (const auto ec = glz::write_json(config, json)) {
        throw std::runtime_error(fmt::format("Failed to serialize config: {}", glz::format_error(ec)));
    }
    return json;
}
