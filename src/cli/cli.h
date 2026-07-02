//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CLI_CLI_H
#define YADDNSC_CLI_CLI_H

#include <string>

// ---------------------------------------------------------------------------
// cli::parse_and_dispatch — parse argv, dispatch non-RUN commands internally,
//                           and tell the caller whether to enter the Manager
//                           RUN loop.
//
// Non-RUN commands (driver, interface, dns, config) are handled entirely
// inside the CLI layer.  The caller only needs to check should_run.
// ---------------------------------------------------------------------------

namespace cli {
    struct CliOutcome {
        bool should_run = false; // true  → main() enters the Manager flow
        int exit_code = 0;
        bool exit_early = false;

        // Fields for the RUN subcommand (only valid when should_run is true)
        std::string config_path = "config.json";
        bool verbose = false;
    };

    // Parse the command line and dispatch non-RUN commands immediately.
    // Never throws — errors and --help/--version are communicated via
    // exit_early / exit_code.
    [[nodiscard]] CliOutcome parse_and_dispatch(int argc, char *argv[]);
} // namespace cli

#endif // YADDNSC_CLI_CLI_H
