//
// Created by Kotarou on 2026/6/30.
//

#include "config.h"

#include <cstdlib>
#include <print>

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "config/config.h"
#include "core/manager.h"
#include "logging_pattern.h"
#include "exception/config_verification.h"
#include "exception/base.h"

namespace Cli {
    void register_config_subcommand(CLI::App &app, const std::string &config_path, int &exit_code) {
        auto *cfg = app.add_subcommand("config", "Configuration management");
        cfg->require_subcommand(1);

        auto *show = cfg->add_subcommand("show", "Print resolved configuration as JSON");
        show->alias("s");
        show->callback([&config_path, &exit_code] {
            exit_code = execute_config_show(config_path);
        });

        auto *test = cfg->add_subcommand("test", "Validate configuration file and exit");
        test->alias("t");
        bool quiet = false;
        test->add_flag("-q,--quiet", quiet, "Suppress success message");
        test->callback([&config_path, &exit_code, &quiet] {
            exit_code = execute_config_test(config_path, quiet);
        });
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
                spdlog::set_pattern(YADDNSC_LOGGING_PATTERN);

                auto config = Config::load_config(config_path);
                // The config test subcommand never calls Manager::run(), so the
                // Scheduler event loop never starts and the stop_source is unused.
                // A default-constructed (still stoppable) stop_source is fine.
                const Manager manager(std::move(config), std::stop_source{});
                manager.load_drivers();
                manager.validate_config();

                if (!quiet) {
                    std::println("Configuration file test passed");
                }
                return EXIT_SUCCESS;
        } catch (const ConfigVerificationException &e) {
            std::println(std::cerr, "Configuration verification failed: {}", e.what());
        } catch (const YaddnscException &) {
            std::println(std::cerr, "Fatal error: unrecoverable exception.");
        } catch (const std::exception &e) {
            std::println(std::cerr, "Failed to validate configuration: {}", e.what());
        }
        return EXIT_FAILURE;
    }
} // namespace Cli
