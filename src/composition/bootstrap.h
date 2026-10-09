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
/// escape dispatch; main() is the process-level boundary for those.
[[nodiscard]] int dispatch(const Cli::Command& command);

}  // namespace Composition

#endif  // YADDNSC_COMPOSITION_BOOTSTRAP_H
