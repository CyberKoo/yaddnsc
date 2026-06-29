//
// Created by Kotarou on 2026/6/29.
//

#include "driver_loader.h"

#include <filesystem>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "driver_manager.h"
#include "util/algorithm.h"

void DriverLoader::load(DriverManager &driver_manager, const Config::config &config) {
    if (config.driver.auto_discover) {
        // Auto-discover all .so files in driver_dir; manual load list is ignored
        if (!config.driver.load.empty()) {
            SPDLOG_WARN("auto_discover is enabled, ignoring manual load list with {} entry(ies)",
                        config.driver.load.size());
        }

        const auto base_dir = std::filesystem::path(config.driver.driver_dir);
        if (!std::filesystem::exists(base_dir)) {
            SPDLOG_WARN("auto_discover enabled but driver_dir '{}' does not exist", config.driver.driver_dir);
            return;
        }
        if (!std::filesystem::is_directory(base_dir)) {
            SPDLOG_WARN("auto_discover enabled but '{}' is not a directory", config.driver.driver_dir);
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
        Util::dedupe(load);

        const auto base_dir = std::filesystem::path(config.driver.driver_dir);
        for (const auto &driver: load) {
            const auto driver_full_path = base_dir.empty() ? std::filesystem::path(driver) : base_dir / driver;
            driver_manager.load_driver(driver_full_path.string());
        }
    }

    if (driver_manager.get_loaded_drivers().empty()) {
        SPDLOG_WARN("No drivers were loaded, DDNS updates will not be performed");
    }
}
