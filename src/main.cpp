//
// Created by Kotarou on 2022/4/5.
//

#include <cstdlib>

#include <spdlog/spdlog.h>

#include "cli/parser.h"
#include "composition/bootstrap.h"
#include "logging_pattern.h"
#include "exception/base.h"
#include "exception/config_verification.h"

// ===========================================================================
// main — DDNS client entry point and top-level error boundary.
//
// Flow:
//   1. Parse argv into a Cli::Command (pure parsing — no side effects).
//      --help/--version and parse errors are consumed by the parser.
//   2. Install the logging pattern, then hand the command to the
//      composition root (Composition::dispatch), the only place where
//      concrete dependencies are assembled.
//   3. Exceptions escaping the RUN path are caught here and logged as
//      fatal errors; diagnostic commands map their own failures to
//      stderr text and exit codes inside dispatch.
// ===========================================================================

int main(int argc, char *argv[]) {
    const auto parsed = Cli::parse(argc, argv);
    if (!parsed.command.has_value()) {
        return parsed.exit_code;
    }

    // Global logging pattern (levels are set per command: run -d, config
    // test -q).
    spdlog::set_pattern(std::string{YADDNSC_LOGGING_PATTERN});

    try {
        return Composition::dispatch(*parsed.command);
    } catch (const ConfigVerificationException &e) {
        SPDLOG_CRITICAL(e.what());
    } catch (const YaddnscException &e) {
        SPDLOG_CRITICAL("Fatal error {}: {}", e.get_name(), e.what());
    } catch (const std::exception &e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    return EXIT_FAILURE;
}
