#ifndef YADDNSC_COMPOSITION_BOOTSTRAP_H
#define YADDNSC_COMPOSITION_BOOTSTRAP_H

#include <spdlog/spdlog.h>  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay

#include "cli/command.h"

namespace Composition {

/// RAII owner of the process-wide log level for one command execution.
///
/// The logging pipeline is a process-wide resource, so is the severity that
/// filters it: every command asks for its level here instead of mutating
/// spdlog itself, and the level the pipeline was initialized with is restored
/// when the command finishes. `run` installs one guard around the whole
/// dispatch, after logging::initialize(), so the level applies to the startup
/// records too.
class LogLevelScope {
public:
    explicit LogLevelScope(const Cli::Command& command);

    ~LogLevelScope() noexcept;

    LogLevelScope(const LogLevelScope&) = delete;
    LogLevelScope& operator=(const LogLevelScope&) = delete;
    LogLevelScope(LogLevelScope&&) = delete;
    LogLevelScope& operator=(LogLevelScope&&) = delete;

private:
    spdlog::level::level_enum previous_level_;
    spdlog::level::level_enum previous_flush_level_;
};

/// Execute a parsed command with process logging and error presentation.
/// Owns the logging pipeline and, for RunCommand, the stop-signal handler until
/// command execution, workers/plugins and logging have all finished. Expected
/// command failures use their command-specific presentation and exit code;
/// escaping defects are logged at this process boundary. Drains logs before
/// returning the process exit code. Call on the main thread.
[[nodiscard]] int run(const Cli::Command& command);

}  // namespace Composition

#endif  // YADDNSC_COMPOSITION_BOOTSTRAP_H
