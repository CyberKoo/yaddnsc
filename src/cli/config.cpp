//
// Created by Kotarou on 2026/6/30.
//

#include "config.h"

#include <CLI/CLI.hpp>

#include <cstdlib>

#include "config/config.h"
#include "config/validator.hpp"
#include "core/driver_loader.h"
#include "core/driver_manager.h"
#include "ip_source/iface_util.h"
#include "exception/base.h"
#include "exception/config_verification.h"

#include "logging_pattern.h"
#include "min_update_interval.h"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

namespace Cli {
    void register_config_subcommand(CLI::App &app, int &exit_code) {
        auto config_path = std::make_shared<std::string>("config.json");

        auto *cfg = app.add_subcommand("config", "Configuration management");
        cfg->require_subcommand(1);
        cfg->add_option("-c,--config", *config_path, "Config file path")
                ->default_str("config.json")
                ->check(CLI::ExistingFile);

        auto *show = cfg->add_subcommand("show", "Print resolved configuration as JSON");
        show->alias("s");
        show->callback([config_path, &exit_code] { exit_code = execute_config_show(*config_path); });

        auto *test = cfg->add_subcommand("test", "Validate configuration file and exit");
        test->alias("t");
        bool quiet = false;
        test->add_flag("-q,--quiet", quiet, "Suppress success message");
        test->callback([config_path, &exit_code, &quiet] { exit_code = execute_config_test(*config_path, quiet); });
    }

    // ── Executors ─────────────────────────────────────────────────────────

    int execute_config_show(const std::string &config_path) {
        auto config = Config::load_config(config_path);
        std::string json;
        if (const auto ec = glz::write_json(config, json)) {
            std::println(std::cerr, "Failed to serialize config: {}", glz::format_error(ec));
            return EXIT_FAILURE;
        }
        std::println("{}", json);
        return EXIT_SUCCESS;
    }

    int execute_config_test(const std::string &config_path, bool quiet) {
        try {
            if (quiet) {
                spdlog::set_level(spdlog::level::off);
            }
            spdlog::set_pattern(std::string{YADDNSC_LOGGING_PATTERN});

            auto config = Config::load_config(config_path);
            DriverManager driver_manager;
            DriverLoader::load(driver_manager, config);
            const auto interfaces = InterfaceUtil::get_interfaces();
            const ConfigValidator<YADDNSC_MIN_UPDATE_INTERVAL> validator(
                driver_manager.get_loaded_drivers(), interfaces);
            validator.validate(config);

            if (!quiet) {
                std::println("Configuration file test passed");
            }
            return EXIT_SUCCESS;
        } catch (const ConfigVerificationException &e) {
            std::println(std::cerr, "Configuration verification failed: {}", e.what());
        } catch (const YaddnscException &e) {
            std::println(std::cerr, "Fatal error: unrecoverable exception: {}", e.what());
        } catch (const std::exception &e) {
            std::println(std::cerr, "Failed to validate configuration: {}", e.what());
        }
        return EXIT_FAILURE;
    }
} // namespace Cli
