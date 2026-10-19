#include "config.h"

#include <filesystem>
#include <string>

#include <glaze/glaze.hpp>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/config/config_exception.h"
#include "infrastructure/config/diagnostics/parse_diagnostic.h"
#include "support/fmt.hpp"

#include "glaze_meta.hpp"  // IWYU pragma: keep

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
