//
// Created by Kotarou on 2026/6/29.
//

#include "driver_loader.h"

#include <filesystem>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "config_cmake.h"
#include "driver_manager.h"
#include "util/algorithm.hpp"

namespace {
    constexpr std::filesystem::path default_driver_dir() {
        return {YADDNSC_DEFAULT_DRIVER_DIR};
    }

    // Resolve the base directory from the optional configuration.
    // Throws if driver_dir is set but empty; falls back to default_driver_dir() otherwise.
    std::filesystem::path resolve_driver_base(const std::optional<std::string> &driver_dir) {
        if (driver_dir.has_value()) {
            if (driver_dir->empty()) {
                throw std::invalid_argument("driver_dir is set but empty in configuration");
            }
            return {driver_dir.value()};
        }
        return default_driver_dir();
    }

    // Given a resolved base directory and a driver name, produce the full path.
    // Relative driver names are prefixed with base_dir; absolute names are used as-is.
    std::filesystem::path resolve_driver_path(const std::filesystem::path &base_dir, const std::string &driver) {
        auto p = std::filesystem::path(driver);
        if (p.is_relative()) {
            return base_dir / p;
        }
        return p;
    }

    void load_auto_discover(DriverManager &driver_manager, const Config::AppConfig &config) {
        if (!config.driver.load.empty()) {
            SPDLOG_WARN("auto_discover is enabled, ignoring manual load list with {} entry(ies)",
                        config.driver.load.size());
        }

        const auto base_dir = resolve_driver_base(config.driver.driver_dir);

        if (!std::filesystem::exists(base_dir)) {
            SPDLOG_WARN("auto_discover enabled but driver_dir '{}' does not exist", base_dir.string());
            return;
        }
        if (!std::filesystem::is_directory(base_dir)) {
            SPDLOG_WARN("auto_discover enabled but '{}' is not a directory", base_dir.string());
            return;
        }

        for (const auto &entry: std::filesystem::directory_iterator(base_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".so") {
                driver_manager.load_driver(entry.path().string());
            }
        }
    }

    void load_manual(DriverManager &driver_manager, const Config::AppConfig &config) {
        auto load = config.driver.load;
        Utils::dedupe(load);

        const auto base_dir = resolve_driver_base(config.driver.driver_dir);

        for (const auto &driver: load) {
            const auto driver_full_path = resolve_driver_path(base_dir, driver);
            driver_manager.load_driver(driver_full_path.string());
        }
    }
} // anonymous namespace

void DriverLoader::load(DriverManager &driver_manager, const Config::AppConfig &config) {
    if (config.driver.auto_discover) {
        load_auto_discover(driver_manager, config);
    } else {
        load_manual(driver_manager, config);
    }

    if (driver_manager.get_loaded_drivers().empty()) {
        SPDLOG_WARN("No drivers were loaded, DDNS updates will not be performed");
    }
}
