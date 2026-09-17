//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_DNS_H
#define YADDNSC_CLI_DNS_H

#include <string>

#include "dns/resolver_catalog.h"

namespace CLI { class App; }

namespace Cli {
    /// Resolve a hostname using the configured resolver and print the results.
    /// @param config_path  Path to the JSON config file.
    /// @param host         Hostname to resolve.
    /// @param type_str     DNS record type string (e.g. "a", "aaaa").
    /// @param catalog      Resolver catalog used to dispatch on the URI schema
    ///                     (production: ResolverCatalog::with_builtins()).
    /// @return             EXIT_SUCCESS or EXIT_FAILURE.
    [[nodiscard]] int execute_dns_resolve(const std::string &config_path, const std::string &host,
                                          const std::string &type_str, const ResolverCatalog &catalog);

    /// Show the configured DNS resolver details.
    [[nodiscard]] int execute_dns_resolver(const std::string &config_path);

    /// Register the "dns" subcommand tree on the given CLI::App.
    /// Owns its own option storage and -c,--config flag internally.
    /// @param catalog  Resolver catalog captured by the resolve callback
    ///                 (production: ResolverCatalog::with_builtins(); tests
    ///                 inject a stub catalog to stay off the network).
    void register_dns_subcommand(CLI::App &app, int &exit_code, ResolverCatalog catalog);
} // namespace Cli

#endif  // YADDNSC_CLI_DNS_H
