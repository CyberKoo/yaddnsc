//
// Created by Kotarou on 2026/6/30.
//

#include "driver.h"

#include <cstdlib>
#include <print>
#include <memory>

#include "config/config.h"
#include "core/driver_manager.h"
#include "core/driver_loader.h"
#include "interfaces/driver.h"

namespace cli {
    // ── Option storage (owned by the callback lambda via shared_ptr) ──────

    namespace {
        struct DriverOpts {
            std::string driver_name;
        };
    }

    void register_driver_subcommand(CLI::App &app, const std::string &config_path, int &exit_code) {
        auto opts = std::make_shared<DriverOpts>();

        auto *driver = app.add_subcommand("driver", "Manage DDNS driver modules");
        driver->require_subcommand(1);

        auto *list = driver->add_subcommand("list", "List all loaded drivers");
        list->callback([&config_path, &exit_code] {
            exit_code = execute_driver_list(config_path);
        });

        auto *info = driver->add_subcommand("info", "Show detailed information about a driver");
        info->add_option("name", opts->driver_name, "Driver name (e.g. simple, cloudflare)")->required();
        info->callback([&config_path, &exit_code, opts] {
            exit_code = execute_driver_info(config_path, opts->driver_name);
        });
    }

    // ── Executors ─────────────────────────────────────────────────────────

    int execute_driver_list(const std::string &config_path) {
        auto config = Config::load_config(config_path);
        DriverManager driver_manager;
        DriverLoader::load(driver_manager, config);

        const auto drivers = driver_manager.get_loaded_drivers();
        if (drivers.empty()) {
            std::println("No drivers loaded.");
            return EXIT_SUCCESS;
        }

        std::println("Loaded drivers ({}):", drivers.size());
        for (const auto &name: drivers) {
            try {
                const auto &driver = driver_manager.get_driver(std::string(name));
                const auto detail = driver.get_detail();
                std::println("  {} \u2014 {} (v{}, by {})", detail.name, detail.description, detail.version,
                             detail.author);
            } catch (const std::exception &e) {
                std::println("  {} \u2014 (failed to query details: {})", name, e.what());
            }
        }
        return EXIT_SUCCESS;
    }

    int execute_driver_info(const std::string &config_path, const std::string &driver_name) {
        auto config = Config::load_config(config_path);
        DriverManager driver_manager;
        DriverLoader::load(driver_manager, config);

        try {
            const auto &driver = driver_manager.get_driver(driver_name);
            const auto detail = driver.get_detail();
            std::println("Name:        {}\n"
                         "Description: {}\n"
                         "Author:      {}\n"
                         "Version:     {}", detail.name, detail.description, detail.author, detail.version);
        } catch (const std::exception &e) {
            std::println(std::cerr, "Error: {}", e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
} // namespace cli
