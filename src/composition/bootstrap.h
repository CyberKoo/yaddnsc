#ifndef YADDNSC_COMPOSITION_BOOTSTRAP_H
#define YADDNSC_COMPOSITION_BOOTSTRAP_H

#include "cli/command.h"

namespace Composition {

/// Execute a parsed command with process logging and error presentation.
/// Owns the logging pipeline and, for RunCommand, the stop-signal handler until
/// command execution, workers/plugins and logging have all finished. Expected
/// command failures use their command-specific presentation and exit code;
/// escaping defects are logged at this process boundary. Drains logs before
/// returning the process exit code. Call on the main thread.
[[nodiscard]] int run(const Cli::Command& command);

}  // namespace Composition

#endif  // YADDNSC_COMPOSITION_BOOTSTRAP_H
