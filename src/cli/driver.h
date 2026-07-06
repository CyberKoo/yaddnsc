//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_DRIVER_H
#define YADDNSC_CLI_DRIVER_H

#include <string>

#include <CLI/CLI.hpp>

namespace Cli {
    /// Load drivers from config and print a summary of every loaded driver.
    int execute_driver_list(const std::string &config_path);

    /// Load drivers from config and print detail for one named driver.
    int execute_driver_info(const std::string &config_path, const std::string &driver_name);

    /// Register the "driver" subcommand tree on the given CLI::App.
    /// Owns its own option storage internally.
    void register_driver_subcommand(CLI::App &app, const std::string &config_path, int &exit_code);
} // namespace Cli

#endif // YADDNSC_CLI_DRIVER_H
