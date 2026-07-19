//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_DNS_H
#define YADDNSC_CLI_DNS_H

#include <string>

namespace CLI { class App; }

namespace Cli {
    /// Resolve a hostname using the configured resolver and print the results.
    /// @param config_path  Path to the JSON config file.
    /// @param host         Hostname to resolve.
    /// @param type_str     DNS record type string (e.g. "a", "aaaa").
    /// @return             EXIT_SUCCESS or EXIT_FAILURE.
    [[nodiscard]] int execute_dns_resolve(const std::string &config_path, const std::string &host,
                                          const std::string &type_str);

    /// Show the configured DNS resolver details.
    [[nodiscard]] int execute_dns_resolver(const std::string &config_path);

    /// Register the "dns" subcommand tree on the given CLI::App.
    /// Owns its own option storage and -c,--config flag internally.
    void register_dns_subcommand(CLI::App &app, int &exit_code);
} // namespace Cli

#endif  // YADDNSC_CLI_DNS_H
