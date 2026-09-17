//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_COMPOSITION_BOOTSTRAP_H
#define YADDNSC_COMPOSITION_BOOTSTRAP_H

#include "cli/command.h"

/// Composition root — the single place where concrete infrastructure
/// implementations are assembled and wired to the application ports.
///
/// Application use cases, CLI handlers and the scheduler never create
/// concrete DNS/HTTP/plugin objects; only this unit does.
namespace Composition {

    /// Execute a parsed command end-to-end.
    /// @return the process exit code.
    ///
    /// Diagnostic commands report every failure via stderr + exit code.
    /// The RUN command lets exceptions escape: main() maps them to fatal
    /// log lines (the top-level error boundary).
    [[nodiscard]] int dispatch(const Cli::Command &command);

} // namespace Composition

#endif // YADDNSC_COMPOSITION_BOOTSTRAP_H
