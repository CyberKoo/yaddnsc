//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CLI_COMMAND_H
#define YADDNSC_CLI_COMMAND_H

#include <string>
#include <variant>

/// Parsed CLI commands — one alternative per user-visible subcommand.
///
/// The parser fills these structs and nothing else: no configuration,
/// plugin, DNS, network or logging side effects happen while parsing.
/// Defaults match the legacy CLI11 option defaults ("config.json", "A").
namespace Cli {

    struct RunCommand {
        std::string config_path{"config.json"}; ///< -c,--config
        bool verbose{false};                    ///< -d,--debug
    };

    struct DriverListCommand {
        std::string config_path{"config.json"};
    };

    struct DriverInfoCommand {
        std::string config_path{"config.json"};
        std::string name; ///< positional: driver name
    };

    struct InterfaceListCommand {};

    struct InterfaceIpCommand {
        std::string name; ///< positional: interface name
    };

    struct DnsResolveCommand {
        std::string config_path{"config.json"};
        std::string host;      ///< positional: hostname to resolve
        std::string type{"A"}; ///< --type (A / AAAA / TXT)
    };

    struct DnsResolverCommand {
        std::string config_path{"config.json"};
    };

    struct ConfigShowCommand {
        std::string config_path{"config.json"};
    };

    struct ConfigTestCommand {
        std::string config_path{"config.json"};
        bool quiet{false}; ///< -q,--quiet: no stdout on success
    };

    struct InfoCommand {};

    /// The parsed command; one active alternative per invocation.
    using Command = std::variant<RunCommand, DriverListCommand, DriverInfoCommand, InterfaceListCommand,
                                 InterfaceIpCommand, DnsResolveCommand, DnsResolverCommand, ConfigShowCommand,
                                 ConfigTestCommand, InfoCommand>;

} // namespace Cli

#endif // YADDNSC_CLI_COMMAND_H
