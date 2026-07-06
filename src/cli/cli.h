//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_CLI_H
#define YADDNSC_CLI_CLI_H

#include <string>

/// Cli::parse_and_dispatch — parse argv, dispatch non-RUN commands internally,
///                           and tell the caller whether to enter the Manager
///                           RUN loop.
///
/// Non-RUN commands (driver, interface, dns, config) are handled entirely
/// inside the CLI layer.  The caller only needs to check should_run.

namespace Cli {
    /// Result of CLI parsing and dispatch.
    struct CliOutcome {
        bool should_run = false; ///< true → main() should enter the Manager flow
        int exit_code = 0;       ///< Exit code (only valid when should_run is false)
        bool exit_early = false; ///< true → CLI consumed the command, caller should not RUN

        /// Fields for the RUN subcommand (only valid when should_run is true)
        std::string config_path = "config.json"; ///< Path to the configuration file
        bool verbose = false;                     ///< Enable debug logging
    };

    /// Parse the command line and dispatch non-RUN commands immediately.
    ///
    /// Non-RUN commands (driver list, interface list, dns resolve, config show,
    /// config test) are executed inline and the result is communicated via
    /// exit_early / exit_code.
    ///
    /// @return CliOutcome describing whether to enter the RUN loop.
    ///
    /// @note Never throws — errors and --help/--version are communicated via
    ///       exit_early / exit_code.
    [[nodiscard]] CliOutcome parse_and_dispatch(int argc, char *argv[]);
} // namespace Cli

#endif // YADDNSC_CLI_CLI_H
