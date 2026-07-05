//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_CONFIG_H
#define YADDNSC_CLI_CONFIG_H

#include <string>

#include <CLI/CLI.hpp>

namespace Cli {
    // Load and print the configuration as JSON.
    int execute_config_show(const std::string &config_path);

    // Load, validate and print the configuration.
    int execute_config_test(const std::string &config_path, bool quiet = false);

    // Register the "config" subcommand tree on the given CLI::App.
    // Owns its own option storage internally.
    void register_config_subcommand(CLI::App &app, const std::string &config_path, int &exit_code);
} // namespace Cli

#endif // YADDNSC_CLI_CONFIG_H
