//
// Unit tests for the `run` command's startup contract
// (src/composition/commands/run.cpp) and for the process-level pieces it
// shares with the other handlers (src/composition/bootstrap.cpp,
// src/composition/assembly.cpp).
//
// Why this file does not call Composition::execute_command
// --------------------------------------------------------
// `run` is the process composition root. Every uncovered line inside
// execute_command sits at or after a step that needs the outside world:
//
//   * the very first statement is internal::load_runtime_config() ->
//     Config::load_config(), whose first operation is
//     std::filesystem::exists() on the configuration file.
//   * the three startup stop checkpoints additionally need a latched
//     coro::StopSignals, i.e. a real pipe plus process signal handlers.
//   * internal::make_dispatcher's std::invalid_argument catch needs a
//     configuration that passes static validation and still violates the
//     factory's contract; the make_gateway lambda needs a driver catalog loaded
//     from a directory; the YaddnscException catch needs a failing dlopen.
// All of those are filesystem / plugin-loading / process-environment work, which
// test/unit/ does not host — test/component/cli_test.cpp already drives
// Composition::run over real temporary configurations and is the right home for
// them.
//
// What is pinned here instead is the pure decision surface around run, so the
// composition root's severity, exit-code and budget contracts cannot drift
// silently: the --log-level vocabulary, the `level <= debug` predicate that
// selects flush-on, the convergence of -d/--debug with --log-level debug, the
// level guard that owns the process-wide spdlog level, the mapping from a
// finished main loop to a process exit code, and the validation budget handed
// to RuntimeServices. Every case is in-memory.
// =============================================================================

#include <array>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "application/services.h"
#include "application/tasks/main_loop.h"
#include "cli/command.h"
#include "composition/assembly.h"
#include "composition/bootstrap.h"

namespace {

/// The five tokens Cli::register_run accepts for --log-level
/// (src/cli/parser.cpp). Nothing else can reach to_log_level in production.
constexpr std::array kCliLogLevels{"error", "warn", "info", "debug", "trace"};

/// Set a known level so a guard's effect (and its restoration) is observable
/// against a fixed baseline, then hand it back to the caller.
class LogLevelBaseline {
public:
    explicit LogLevelBaseline(spdlog::level::level_enum level) : previous_(spdlog::get_level()) {
        spdlog::set_level(level);
    }

    ~LogLevelBaseline() { spdlog::set_level(previous_); }

    LogLevelBaseline(const LogLevelBaseline&) = delete;
    LogLevelBaseline& operator=(const LogLevelBaseline&) = delete;

private:
    spdlog::level::level_enum previous_;
};

}  // namespace

// ===========================================================================
//  --log-level vocabulary
// ===========================================================================

TEST(RunCommand, EveryCliLogLevelTokenMapsToItsIntendedSeverity) {
    const std::array<std::pair<std::string_view, spdlog::level::level_enum>, 5> expected{{
        {"error", spdlog::level::err},
        {"warn", spdlog::level::warn},
        {"info", spdlog::level::info},
        {"debug", spdlog::level::debug},
        {"trace", spdlog::level::trace},
    }};

    for (const auto& [token, level] : expected) {
        SCOPED_TRACE(token);
        EXPECT_EQ(Composition::internal::to_log_level(token), level);
    }
}

TEST(RunCommand, CliLogLevelTokensMapToDistinctSeverities) {
    // A collision would make two accepted --log-level values behave
    // identically, so no token may share a severity with another.
    std::set<int> distinct;
    for (const std::string_view token : kCliLogLevels) {
        SCOPED_TRACE(token);
        distinct.insert(static_cast<int>(Composition::internal::to_log_level(token)));
    }
    EXPECT_EQ(distinct.size(), kCliLogLevels.size());
}

// ===========================================================================
//  The `level <= debug` predicate that turns flush-on on
// ===========================================================================

TEST(RunCommand, FlushOnAppliesExactlyToTraceAndDebug) {
    // Foreground sessions need every record flushed so output survives a crash
    // and appears live through pipes and files; the quieter levels keep spdlog's
    // default buffering.
    for (const std::string_view token : {"trace", "debug"}) {
        SCOPED_TRACE(token);
        EXPECT_LE(Composition::internal::to_log_level(token), spdlog::level::debug);
    }
    for (const std::string_view token : {"info", "warn", "error"}) {
        SCOPED_TRACE(token);
        EXPECT_GT(Composition::internal::to_log_level(token), spdlog::level::debug);
    }
}

// ===========================================================================
//  -d/--debug versus --log-level debug
// ===========================================================================

TEST(RunCommand, VerboseFlagConvergesOnTheDebugToken) {
    // `-d` and `--log-level debug` must leave the process at the same severity
    // with the same flush threshold; --log-level simply wins when both are given.
    EXPECT_EQ(Composition::internal::to_log_level("debug"), spdlog::level::debug);
    EXPECT_LE(Composition::internal::to_log_level("debug"), spdlog::level::debug);
}

// ===========================================================================
//  Startup input surface
// ===========================================================================

TEST(RunCommand, DefaultsLeaveBothLoggingBranchesUnset) {
    // The default RunCommand takes neither branch of the startup logging if:
    // an empty log_level means to_log_level is never consulted and verbose is
    // off, so the process keeps whatever level logging initialization chose.
    const Cli::RunCommand command;
    EXPECT_TRUE(command.log_level.empty());
    EXPECT_FALSE(command.verbose);
}

TEST(RunCommand, LogLevelTakesPrecedenceOverVerbose) {
    // Both flags set is a legal command line; --log-level is documented as
    // overriding -d, so the mapped severity is the one that must survive.
    const Cli::RunCommand command{.config_path = "config.json", .verbose = true, .log_level = "warn"};
    EXPECT_TRUE(command.verbose);
    EXPECT_EQ(Composition::internal::to_log_level(command.log_level), spdlog::level::warn);
    EXPECT_GT(Composition::internal::to_log_level(command.log_level), spdlog::level::debug);
}

// ===========================================================================
//  Budgets handed to the runtime services
// ===========================================================================

TEST(RunCommand, ValidationBudgetMatchesTheRunPathPolicy) {
    // run.cpp wires internal::UPDATE_POLICY into RuntimeServices; its
    // driver_validation_budget is the injected deadline from
    // CONFIG_VALIDATION_BUDGET, never a re-defaulted copy.
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.driver_validation_budget, std::chrono::milliseconds(5000));
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.driver_validation_budget,
              Composition::internal::CONFIG_VALIDATION_BUDGET);
}

TEST(RunCommand, UpdatePolicyKeepsTheLibraryDefaultsOutsideTheValidationBudget) {
    const app::UpdatePolicy defaults;
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.update_budget, defaults.update_budget);
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.dns_read_budget, defaults.dns_read_budget);
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.ip_source_budget, defaults.ip_source_budget);
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.driver_validation_budget,
              Composition::internal::CONFIG_VALIDATION_BUDGET);
}

// ===========================================================================
//  The process-wide level guard (src/composition/bootstrap.cpp)
// ===========================================================================

TEST(LogLevelScope, ExplicitLogLevelTokenIsApplied) {
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope scope{Cli::RunCommand{.log_level = "warn"}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::warn);
    }
    EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
}

TEST(LogLevelScope, LogLevelWinsOverTheDebugFlag) {
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope scope{Cli::RunCommand{.verbose = true, .log_level = "error"}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::err);
    }
    EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
}

TEST(LogLevelScope, DebugFlagAloneSelectsDebug) {
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope scope{Cli::RunCommand{.verbose = true}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::debug);
    }
    EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
}

TEST(LogLevelScope, DebugLevelTurnsOnFlushAndTheLevelIsRestored) {
    // Foreground debug sessions must survive a crash, and must leave no global
    // residue behind for whatever runs next in the same process.
    const auto previous_flush = spdlog::default_logger()->flush_level();
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope scope{Cli::RunCommand{.log_level = "debug"}};
        EXPECT_EQ(spdlog::default_logger()->flush_level(), spdlog::level::debug);
    }
    EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
    EXPECT_EQ(spdlog::default_logger()->flush_level(), previous_flush);
}

TEST(LogLevelScope, QuietConfigTestSilencesDiagnosticsOnlyForItsOwnRun) {
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope scope{Cli::ConfigTestCommand{.quiet = true}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::off);
    }
    EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
}

TEST(LogLevelScope, CommandsWithoutALevelRequestLeaveThePipelineAlone) {
    const LogLevelBaseline baseline{spdlog::level::info};
    {
        const Composition::LogLevelScope config_test{Cli::ConfigTestCommand{}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
    }
    {
        const Composition::LogLevelScope run{Cli::RunCommand{}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
    }
    {
        const Composition::LogLevelScope info{Cli::InfoCommand{}};
        EXPECT_EQ(spdlog::get_level(), spdlog::level::info);
    }
}

// ===========================================================================
//  Exit-code ownership: the loop names the outcome, the handler maps it
// ===========================================================================

TEST(RunCommand, OnlyGracefulShutdownExitsZero) {
    EXPECT_EQ(Composition::internal::run_exit_code(app::MainLoopOutcome::GracefulShutdown), EXIT_SUCCESS);
    EXPECT_EQ(Composition::internal::run_exit_code(app::MainLoopOutcome::ConfigurationRejected), EXIT_FAILURE);
    EXPECT_EQ(Composition::internal::run_exit_code(app::MainLoopOutcome::UpdateTasksEnded), EXIT_FAILURE);
}

TEST(RunCommand, EveryNonShutdownOutcomeFailsTheProcess) {
    // A supervisor with Restart=on-failure must not restart into a rejected
    // configuration, and it must not leave a husk behind after every subdomain
    // task died, so neither outcome may report success.
    for (const auto outcome : {app::MainLoopOutcome::ConfigurationRejected, app::MainLoopOutcome::UpdateTasksEnded}) {
        SCOPED_TRACE(static_cast<int>(outcome));
        EXPECT_NE(Composition::internal::run_exit_code(outcome), EXIT_SUCCESS);
    }
}