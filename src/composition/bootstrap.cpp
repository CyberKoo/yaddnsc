#include "bootstrap.h"

#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <variant>

#include <spdlog/spdlog.h>

#include "cli/presenter.h"
#include "composition/commands/diagnostic_commands.h"
#include "composition/commands/run.h"
#include "composition/startup_signals.h"
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

    // The run command's counting handler is still installed (the Loop's
    // destructor restored it), so a repeated Ctrl-C during the drain was
    // counted rather than swallowed; force the same exit status the in-loop
    // watcher produced. Out-of-loop arrivals are all this sees — the loop-phase
    // count lived and acted inside the run root.
    if (startup_signal_counts().sigint >= 2) {
        ::_exit(128 + SIGINT);
    }
    return exit_code;
}
