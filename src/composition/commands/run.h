#ifndef YADDNSC_COMPOSITION_COMMANDS_RUN_H
#define YADDNSC_COMPOSITION_COMMANDS_RUN_H

namespace Cli {
struct RunCommand;
}  // namespace Cli

namespace coro {
class StopSignals;
}

namespace Composition {
/// Assemble and run with the process boundary's signal owner. Expected startup
/// failures are logged; defects propagate to that boundary. Logging must be
/// initialized, and signals must outlive command execution and logging drain.
[[nodiscard]] int execute_command(const Cli::RunCommand& command, const coro::StopSignals& signals);
}  // namespace Composition
#endif  // YADDNSC_COMPOSITION_COMMANDS_RUN_H
