//
// Created by Kotarou on 2026/6/29.
//

#include "driver_loader.h"

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "domain/config/runtime_config.h"
#include "infrastructure/config/config_verification_exception.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/plugin_load_exception.h"
#include "support/util/algorithm.hpp"

#include "config_cmake.h"

namespace {
[[nodiscard]] std::filesystem::path default_driver_dir() {
    return {YADDNSC_DEFAULT_DRIVER_DIR};
}

// Resolve the base directory from the optional configuration.
// Throws if driver_dir is set but empty; falls back to default_driver_dir() otherwise.
[[nodiscard]] std::filesystem::path resolve_driver_base(const std::optional<std::filesystem::path>& driver_dir) {
    if (driver_dir.has_value()) {
        if (driver_dir->empty()) {
            throw ConfigVerificationException("driver_dir is set but empty in configuration");
        }
        return driver_dir.value();
    }
    return default_driver_dir();
}

// Given a resolved base directory and a driver name, produce the full path.
// Relative driver names are prefixed with base_dir; absolute names are used as-is.
[[nodiscard]]
std::filesystem::path resolve_driver_path(const std::filesystem::path& base_dir, const std::string& driver) {
    auto p = std::filesystem::path(driver);
    if (p.is_relative()) {
        return base_dir / p;
    }
    return p;
}

void load_auto_discover(DriverCatalog& driver_catalog, const domain::DriverSettings& settings) {
    if (!settings.load.empty()) {
        SPDLOG_WARN("auto_discover is enabled, ignoring manual load list with {} entry(ies)", settings.load.size());
    }

    const auto base_dir = resolve_driver_base(settings.driver_dir);

    if (!std::filesystem::exists(base_dir)) {
        SPDLOG_WARN("auto_discover enabled but driver_dir '{}' does not exist", base_dir.string());
        return;
    }
    if (!std::filesystem::is_directory(base_dir)) {
        SPDLOG_WARN("auto_discover enabled but '{}' is not a directory", base_dir.string());
        return;
    }

    // The directory contents are not under our control: a single
    // unrelated or corrupted library (other plugins, partial copies) must
    // not abort startup — skip it with a warning and keep going.
    // Manual loads (load_manual) still fail fast: an explicitly
    // configured driver that cannot load is a configuration error.
    try {
        for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".so") {
                continue;
            }
            try {
                driver_catalog.load_driver(entry.path().string());
            } catch (const PluginLoadException& e) {
                SPDLOG_WARN("Skipping invalid driver '{}': {}", entry.path().filename().string(), e.what());
            } catch (const std::exception& e) {
                SPDLOG_WARN("Skipping driver '{}': {}", entry.path().filename().string(), e.what());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // Directory removed / permissions changed during iteration.
        SPDLOG_WARN("Failed to iterate driver directory '{}': {}", base_dir.string(), e.what());
    }
}

void load_manual(DriverCatalog& driver_catalog, const domain::DriverSettings& settings) {
    auto load = settings.load;
    Utils::dedupe(load);

    const auto base_dir = resolve_driver_base(settings.driver_dir);

    for (const auto& driver : load) {
        const auto driver_full_path = resolve_driver_path(base_dir, driver);
        driver_catalog.load_driver(driver_full_path.string());
    }
}
}  // anonymous namespace

void DriverLoader::load(DriverCatalog& driver_catalog, const domain::DriverSettings& settings) {
    if (settings.auto_discover) {
        load_auto_discover(driver_catalog, settings);
    } else {
        load_manual(driver_catalog, settings);
    }

    const auto loaded = driver_catalog.get_loaded_drivers();

    if (loaded.empty()) {
        SPDLOG_WARN("No drivers were loaded, DDNS updates will not be performed");
    } else {
        SPDLOG_INFO("Loaded {} driver(s)", loaded.size());
    }
}
