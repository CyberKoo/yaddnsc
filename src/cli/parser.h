//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CLI_PARSER_H
#define YADDNSC_CLI_PARSER_H

#include <cstdlib>
#include <optional>

#include "command.h"

namespace Cli {

    /// Result of parsing the command line.
    struct ParseResult {
        std::optional<Command> command;  ///< nullopt → nothing to execute
        int exit_code{EXIT_SUCCESS};     ///< exit code to use when command is nullopt
    };

    /// Parse argv into a Command. Pure parsing: callbacks only fill the
    /// command variant — no configuration loading, no plugin loading, no
    /// DNS or interface queries, no global log-level changes, and no business
    /// output. --help/--version and parse errors print through CLI11 and
    /// return a null command with the corresponding exit code.
    [[nodiscard]] ParseResult parse(int argc, char *argv[]);

} // namespace Cli

#endif // YADDNSC_CLI_PARSER_H
