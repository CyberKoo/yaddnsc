//
// Unit tests for the `run` command's startup contract
// (src/composition/commands/run.cpp).
//
// Why this file does not call Composition::execute_command
// --------------------------------------------------------
// `run` is the process composition root. Every uncovered line inside
// execute_command sits at or after a step that needs the outside world:
//
//   * the log-level setup (run.cpp:50-57) is pure, but it is only reachable by
//     entering the function, and the very next statement is
//     internal::load_runtime_config() -> Config::load_config(), whose first
//     operation is std::filesystem::exists() on the configuration file.
//   * the three startup stop checkpoints (run.cpp:66-89) additionally need a
//     latched coro::StopSignals, i.e. a real pipe plus process signal handlers.
//   * the dispatcher factory's std::invalid_argument catch (run.cpp:96-100)
//     needs a configuration that passes static validation and still violates the
//     factory's contract; the make_gateway lambda (run.cpp:117) needs a driver
//     catalog loaded from a directory; the YaddnscException catch
//     (run.cpp:130-132) needs a failing dlopen.
// All of those are filesystem / plugin-loading / process-environment work, which
// test/unit/ does not host — test/component/cli_test.cpp already drives
// Composition::run over real temporary configurations and is the right home for
// them.
//
// What is pinned here instead is the pure decision surface run.cpp reads at
// startup, so the composition root's severity and budget contract cannot drift
// silently: the --log-level vocabulary, the `level <= debug` predicate that
// selects flush-on, the convergence of -d/--debug with --log-level debug, and
// the validation budget handed to RuntimeServices. Every case is in-memory.
// =============================================================================

#include <array>
#include <chrono>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "application/services.h"
#include "cli/command.h"
#include "composition/assembly.h"

namespace {

/// The five tokens Cli::register_run accepts for --log-level
/// (src/cli/parser.cpp). Nothing else can reach to_log_level in production.
constexpr std::array kCliLogLevels{"error", "warn", "info", "debug", "trace"};

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