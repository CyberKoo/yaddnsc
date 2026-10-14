#include "bootstrap.h"

#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>
#include <variant>

#include <spdlog/spdlog.h>

#include "cli/presenter.h"
#include "composition/commands/diagnostic_commands.h"
#include "composition/commands/run.h"
#include "coro/stop_signals.h"
#include "infrastructure/logging/async_logging.h"

namespace {
struct Dispatch {
    const std::optional<coro::StopSignals>& signals;

    int operator()(const Cli::RunCommand& command) const {
        return Composition::execute_command(command, signals.value());
    }

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

int Composition::run(const Cli::Command& command) {
    // This is the sole command entry point. Run's handler owner outlives
    // command execution, worker/plugin cleanup and logging shutdown.
    std::optional<coro::StopSignals> signals;
    int exit_code = EXIT_FAILURE;
    try {
        // Install handlers before logging starts its worker thread.
        if (std::holds_alternative<Cli::RunCommand>(command)) {
            signals.emplace();
        }
        logging::initialize();
        exit_code = std::visit(Dispatch{signals}, command);
    } catch (const std::exception& error) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", error.what());
    }
    logging::shutdown();
    return exit_code;
}
