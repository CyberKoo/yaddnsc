//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_CONFIG_H
#define YADDNSC_CLI_CONFIG_H

#include <string>

namespace CLI { class App; }

namespace Cli {
    /// Load and print the configuration as JSON.
    [[nodiscard]] int execute_config_show(const std::string &config_path);

    /// Load, validate and print the configuration.
    /// @param config_path  Path to the JSON config file.
    /// @param quiet        When true, only indicate success/failure via exit code.
    /// @return             EXIT_SUCCESS or EXIT_FAILURE.
    [[nodiscard]] int execute_config_test(const std::string &config_path, bool quiet = false);

    /// Register the "config" subcommand tree on the given CLI::App.
    /// Owns its own option storage and -c,--config flag internally.
    void register_config_subcommand(CLI::App &app, int &exit_code);
} // namespace Cli

#endif  // YADDNSC_CLI_CONFIG_H
