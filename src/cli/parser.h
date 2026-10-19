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

/// Parse argv into a Command. Callbacks only fill the command variant — no
/// configuration loading, no plugin loading, no DNS or interface queries and
/// no global log-level changes happen here. --help/--version and parse errors
/// print through CLI11; every other rejection — including the no-subcommand
/// fallback — is presented through Cli::present_error, so the wording and the
/// exit code stay the presenter's single source.
[[nodiscard]] ParseResult parse(int argc, char* argv[]);

}  // namespace Cli

#endif  // YADDNSC_CLI_PARSER_H
