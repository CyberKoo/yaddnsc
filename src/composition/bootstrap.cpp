#include "bootstrap.h"

#include <cstdlib>
#include <exception>
#include <variant>

#include <spdlog/spdlog.h>

#include "cli/presenter.h"
#include "composition/commands/diagnostics.h"
#include "composition/commands/run.h"
#include "infrastructure/logging/async_logging.h"

namespace {
struct Dispatch {
    // Run owns startup logging; defects reach run()'s process boundary.
    int operator()(const Cli::RunCommand& command) const { return Composition::execute_command(command); }

    // Diagnostic assembly failures share the plain stderr presentation policy.
    template<typename Command>
    int operator()(const Command& command) const {
        try {
            return Composition::execute_command(command);
        } catch (const std::exception& error) {
            return Cli::present_error(error);
        }
    }
};
}  // namespace

int Composition::dispatch(const Cli::Command& command) {
    return std::visit(Dispatch{}, command);
}

int Composition::run(const Cli::Command& command) {
    // Async logging is installed before any command runs (design §6.4): the
    // default logger, the app::LoggerPort facade (SpdlogLogger) and every direct
    // SPDLOG_* call site share one pipeline. Levels are still set per command
    // (run -d, config test -q).
    logging::initialize();

    int exit_code = EXIT_FAILURE;
    try {
        exit_code = dispatch(command);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", e.what());
    }

    // Drain the async pipeline and report any dropped records before returning;
    // this single point also flushes the fatal boundary's last record.
    logging::shutdown();
    return exit_code;
}
