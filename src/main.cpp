#include <cstdlib>
#include <exception>
#include <optional>

#include <spdlog/spdlog.h>

#include "cli/parser.h"
#include "composition/bootstrap.h"
#include "infrastructure/logging/async_logging.h"

// ===========================================================================
// main — DDNS client entry point and the process-level defect boundary.
//
// Flow:
//   1. Parse argv into a Cli::Command. The parser owns all pre-command
//      diagnostics: --help/--version and parse errors are consumed there.
//   2. Install the asynchronous logging pipeline, then hand the command to the
//      composition root (Composition::dispatch), the only place where
//      concrete dependencies are assembled.
//   3. Command handlers are total for their expected failures: each maps them
//      to its own presentation and exit code inside dispatch. Only defects
//      escape dispatch and are caught here as fatal log lines. The async
//      pipeline is drained (and dropped records reported) before returning.
// ===========================================================================

int main(int argc, char* argv[]) {
    const auto parsed = Cli::parse(argc, argv);
    if (!parsed.command.has_value()) {
        return parsed.exit_code;
    }

    // Async logging is installed before any command runs (design §6.4): the
    // default logger, the app::LoggerPort facade (SpdlogLogger) and every direct
    // SPDLOG_* call site share one pipeline. Levels are still set per command
    // (run -d, config test -q).
    logging::initialize();

    int exit_code = EXIT_FAILURE;
    try {
        exit_code = Composition::dispatch(*parsed.command);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    // Drain the async pipeline and report any dropped records before returning;
    // this single point also flushes the fatal boundary's last record.
    logging::shutdown();
    return exit_code;
}
