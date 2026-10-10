#ifndef YADDNSC_COMPOSITION_BOOTSTRAP_H
#define YADDNSC_COMPOSITION_BOOTSTRAP_H

#include "cli/command.h"

/// Routes commands to their composition handlers and selects the error presentation.
namespace Composition {

/// Execute a parsed command end-to-end.
/// @return the process exit code.
///
/// Every command handler is total for its expected failures: each maps them
/// to its own presentation (run logs through the async pipeline; diagnostic
/// commands print "Error: <what>" on stderr) and exit code. Only defects
/// escape dispatch; run() is the process-level boundary for those.
[[nodiscard]] int dispatch(const Cli::Command& command);

/// Execute a parsed command with the process-wide logging pipeline installed.
/// @return the process exit code.
///
/// Owns the async logging lifetime around dispatch(): an exception escaping
/// dispatch is recorded as a fatal log line, and the pipeline is drained (with
/// dropped records reported) before returning, so the fatal record is flushed.
[[nodiscard]] int run(const Cli::Command& command);

}  // namespace Composition

#endif  // YADDNSC_COMPOSITION_BOOTSTRAP_H
