#ifndef YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTICS_H
#define YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTICS_H
#include "cli/command.h"

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
#endif  // YADDNSC_COMPOSITION_COMMANDS_DIAGNOSTICS_H
