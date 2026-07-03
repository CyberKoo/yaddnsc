//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_DNS_H
#define YADDNSC_CLI_DNS_H

#include <string>

#include <CLI/CLI.hpp>

namespace Cli {
    // Resolve a hostname using the configured resolver.
    int execute_dns_resolve(const std::string &config_path, const std::string &host, const std::string &type_str);

    // Show the configured DNS resolver details.
    int execute_dns_resolver(const std::string &config_path);

    // Register the "dns" subcommand tree on the given CLI::App.
    // Owns its own option storage internally.
    void register_dns_subcommand(CLI::App &app, const std::string &config_path, int &exit_code);
} // namespace Cli

#endif // YADDNSC_CLI_DNS_H
