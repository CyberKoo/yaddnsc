#ifndef YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTIC_COMMANDS_H
#define YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTIC_COMMANDS_H

namespace Cli {
struct ConfigShowCommand;
struct ConfigTestCommand;
struct DnsResolveCommand;
struct DnsResolverCommand;
struct DriverInfoCommand;
struct DriverListCommand;
struct InfoCommand;
struct InterfaceIpCommand;
struct InterfaceListCommand;
}  // namespace Cli

namespace Composition {
/// Assemble diagnostics; config test presents its own failures, others propagate
/// to the dispatch presentation boundary. Main-thread entry points.
[[nodiscard]] int execute_command(const Cli::DriverListCommand& command);
[[nodiscard]] int execute_command(const Cli::DriverInfoCommand& command);
[[nodiscard]] int execute_command(const Cli::InterfaceListCommand& command);
[[nodiscard]] int execute_command(const Cli::InterfaceIpCommand& command);
[[nodiscard]] int execute_command(const Cli::DnsResolveCommand& command);
[[nodiscard]] int execute_command(const Cli::DnsResolverCommand& command);
[[nodiscard]] int execute_command(const Cli::ConfigShowCommand& command);
[[nodiscard]] int execute_command(const Cli::ConfigTestCommand& command);
[[nodiscard]] int execute_command(const Cli::InfoCommand& command);
}  // namespace Composition
#endif  // YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTIC_COMMANDS_H
