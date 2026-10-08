//
// Created by Kotarou on 2022/4/5.
//

#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "cli/parser.h"
#include "composition/bootstrap.h"
#include "infrastructure/config/config_verification_exception.h"
#include "support/exception.h"

// ===========================================================================
// main — DDNS client entry point and top-level error boundary.
//
// Flow:
//   1. Parse argv into a Cli::Command (pure parsing — no side effects).
//      --help/--version and parse errors are consumed by the parser.
//   2. Install the asynchronous logging pipeline, then hand the command to the
//      composition root (Composition::dispatch), the only place where
//      concrete dependencies are assembled.
//   3. Exceptions escaping the RUN path are caught here and logged as
//      fatal errors; diagnostic commands map their own failures to
//      stderr text and exit codes inside dispatch. The async pipeline is
//      drained (and dropped records reported) before returning.
// ===========================================================================

int main(int argc, char* argv[]) {
    const auto parsed = Cli::parse(argc, argv);
    if (!parsed.command.has_value()) {
        return parsed.exit_code;
    }

    // Async logging is installed before any command runs (design §6.4): the
    // default logger, the Logger facade (SpdlogLogger) and every direct
    // SPDLOG_* call site share one pipeline. Levels are still set per command
    // (run -d, config test -q).
    Composition::initialize_logging();

    int exit_code = EXIT_FAILURE;
    try {
        exit_code = Composition::dispatch(*parsed.command);
    } catch (const ConfigVerificationException& e) {
        SPDLOG_CRITICAL(e.what());
    } catch (const YaddnscException& e) {
        SPDLOG_CRITICAL("Fatal error {}: {}", e.get_name(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    // Drain the async pipeline and report any dropped records before returning;
    // this single point also flushes the fatal boundary's last record.
    Composition::shutdown_logging();
    return exit_code;
}
