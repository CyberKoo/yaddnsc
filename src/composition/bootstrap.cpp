#include "bootstrap.h"

#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <spdlog/spdlog.h>

#include "cli/presenter.h"
#include "composition/assembly.h"
#include "composition/commands/diagnostic_commands.h"
#include "composition/commands/run.h"
#include "coro/stop_signals.h"
#include "infrastructure/logging/async_logging.h"

namespace {
/// The severity a command asks of the logging pipeline, or nullopt when it
/// defers to the level initialization chose. Only two commands have an opinion:
/// `run` selects through --log-level, which overrides -d, and `config test
/// --quiet` silences the diagnostics its sweep would otherwise emit.
struct RequestedLevel {
    [[nodiscard]] std::optional<spdlog::level::level_enum> operator()(const Cli::RunCommand& run) const {
        if (!run.log_level.empty()) {
            return Composition::internal::to_log_level(run.log_level);
        }
        if (run.verbose) {
            return spdlog::level::debug;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<spdlog::level::level_enum> operator()(const Cli::ConfigTestCommand& test) const {
        return test.quiet ? std::optional{spdlog::level::off} : std::nullopt;
    }

    template<typename Command>
    [[nodiscard]] std::optional<spdlog::level::level_enum> operator()(const Command&) const {
        return std::nullopt;
    }
};

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

Composition::LogLevelScope::LogLevelScope(const Cli::Command& command)
    : previous_level_(spdlog::get_level()), previous_flush_level_(spdlog::default_logger()->flush_level()) {
    const auto requested = std::visit(RequestedLevel{}, command);
    if (!requested.has_value()) {
        return;
    }
    spdlog::set_level(*requested);
    if (*requested <= spdlog::level::debug) {
        // Debug runs are foreground sessions: flush every record so the output
        // survives a crash and appears live through pipes and files.
        spdlog::flush_on(spdlog::level::debug);
    }
    SPDLOG_DEBUG("Log level set to {}", spdlog::level::to_string_view(*requested));
}

Composition::LogLevelScope::~LogLevelScope() noexcept {
    spdlog::set_level(previous_level_);
    spdlog::flush_on(previous_flush_level_);
}

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
        {
            LogLevelScope log_level{command};
            exit_code = std::visit(Dispatch{signals}, command);
        }
    } catch (const std::exception& error) {
        SPDLOG_CRITICAL("Unhandled exception. Error: {}", error.what());
    }
    logging::shutdown();
    return exit_code;
}
