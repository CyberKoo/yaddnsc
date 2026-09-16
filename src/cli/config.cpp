//
// Created by Kotarou on 2026/6/30.
//

#include "config.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <print>
#include <string_view>

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

namespace {
    /// `config show` redaction rule (registered as an intentional change in
    /// refactor/phase-0-baseline.md): an object member is sensitive when its
    /// lower-cased key contains "token", "password", "secret" or "key".
    /// The whole value is replaced with "***" regardless of its type.
    /// Substring matching may over-redact (e.g. a key like "monkey"); that is
    /// accepted for a diagnostic view — the config file keeps the real values.
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

    void register_config_subcommand(CLI::App &app, int &exit_code) {
        auto *cfg = app.add_subcommand("config", "Configuration management");
        cfg->require_subcommand(1);

        auto *show = cfg->add_subcommand("show", "Print resolved configuration as JSON");
        show->alias("s");
        auto show_path = std::make_shared<std::string>("config.json");
        show->add_option("-c,--config", *show_path, "Config file path")
                ->default_str("config.json")
                ->check(CLI::ExistingFile);
        show->callback([show_path, &exit_code] { exit_code = execute_config_show(*show_path); });

        auto *test = cfg->add_subcommand("test", "Validate configuration file and exit");
        test->alias("t");
        auto test_path = std::make_shared<std::string>("config.json");
        // The callback outlives this function (CLI11 defers execution until
        // parse), so every captured value must be owned by a shared_ptr.
        auto quiet = std::make_shared<bool>(false);
        test->add_flag("-q,--quiet", *quiet, "Suppress success message");
        test->add_option("-c,--config", *test_path, "Config file path")
                ->default_str("config.json")
                ->check(CLI::ExistingFile);
        test->callback([test_path, quiet, &exit_code] { exit_code = execute_config_test(*test_path, *quiet); });
    }

    // ── Executors ─────────────────────────────────────────────────────────

    int execute_config_show(const std::string &config_path) {
        auto config = Config::load_config(config_path);
        for (auto &domain: config.domains) {
            for (auto &subdomain: domain.subdomains) {
                redact_sensitive_fields(subdomain.driver_param);
            }
        }
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
