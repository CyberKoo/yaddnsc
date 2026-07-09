//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_INTERFACE_H
#define YADDNSC_CLI_INTERFACE_H

#include <string>

#include <CLI/CLI.hpp>

namespace Cli {
    /// List all network interfaces and their addresses.
    [[nodiscard]] int execute_interface_list();

    /// Show IP addresses for a specific interface.
    [[nodiscard]] int execute_interface_ip(const std::string &interface_name);

    /// Register the "interface" subcommand tree on the given CLI::App.
/// Owns its own option storage internally.
    void register_interface_subcommand(CLI::App &app, int &exit_code);
} // namespace Cli

#endif  // YADDNSC_CLI_INTERFACE_H
