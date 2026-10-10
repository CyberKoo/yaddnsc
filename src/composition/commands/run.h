#ifndef YADDNSC_COMPOSITION_COMMANDS_RUN_H
#define YADDNSC_COMPOSITION_COMMANDS_RUN_H

namespace Cli {
struct RunCommand;
}  // namespace Cli

namespace Composition {
/// Assemble and run the application. Expected startup failures are logged;
/// defects propagate to main. Call on the main thread with logging initialized.
[[nodiscard]] int execute_command(const Cli::RunCommand& command);
}  // namespace Composition
#endif  // YADDNSC_COMPOSITION_COMMANDS_RUN_H
