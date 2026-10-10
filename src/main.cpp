#include <optional>

#include "cli/parser.h"
#include "composition/bootstrap.h"

// ===========================================================================
// main — DDNS client entry point.
//
// Flow:
//   1. Parse argv into a Cli::Command. The parser owns all pre-command
//      diagnostics: --help/--version and parse errors are consumed there.
//   2. Hand the command to the composition root (Composition::run), the only
//      place where concrete dependencies are assembled. It owns the logging
//      pipeline's lifetime and the process-level defect boundary.
// ===========================================================================

int main(int argc, char* argv[]) {
    const auto parsed = Cli::parse(argc, argv);
    if (!parsed.command.has_value()) {
        return parsed.exit_code;
    }
    return Composition::run(*parsed.command);
}
