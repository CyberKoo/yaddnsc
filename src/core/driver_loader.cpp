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
    std::filesystem::path default_driver_dir() {
        return {YADDNSC_DEFAULT_DRIVER_DIR};
    }

    std::filesystem::path make_driver_path(const std::optional<std::string> &driver_dir, const std::string &driver) {
        if (driver_dir.has_value()) {
            if (driver_dir->empty()) {
                throw std::invalid_argument("driver_dir is set but empty in configuration");
            }
            // Explicit non-empty driver_dir: always prepend it.
            return std::filesystem::path(driver_dir.value()) / driver;
        }

        // No explicit driver_dir: treat relative paths relative to the
        // default install directory, absolute paths as-is.
        auto p = std::filesystem::path(driver);
        if (p.is_relative()) {
            return default_driver_dir() / p;
        }
        return p;
    }
} // anonymous namespace

void DriverLoader::load(DriverManager &driver_manager, const Config::AppConfig &config) {
    if (config.driver.auto_discover) {
        // Auto-discover all .so files in driver_dir; manual load list is ignored
        if (!config.driver.load.empty()) {
            SPDLOG_WARN("auto_discover is enabled, ignoring manual load list with {} entry(ies)",
                        config.driver.load.size());
        }

        const auto base_dir = [&]() -> std::filesystem::path {
            if (config.driver.driver_dir.has_value()) {
                const auto &dir = config.driver.driver_dir.value();
                if (dir.empty()) {
                    throw std::invalid_argument("driver_dir is set but empty in configuration");
                }
                return {dir};
            }
            return default_driver_dir();
        }();

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
    } else {
        // Manual load list
        auto load = config.driver.load;
        Utils::dedupe(load);

        for (const auto &driver: load) {
            const auto driver_full_path = make_driver_path(config.driver.driver_dir, driver);
            driver_manager.load_driver(driver_full_path.string());
        }
    }

    if (driver_manager.get_loaded_drivers().empty()) {
        SPDLOG_WARN("No drivers were loaded, DDNS updates will not be performed");
    }
}
