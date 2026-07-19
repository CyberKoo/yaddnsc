//
// Created by Kotarou on 2026/7/6.
//

#ifndef YADDNSC_CLI_INFO_H
#define YADDNSC_CLI_INFO_H

namespace CLI { class App; }

namespace Cli {
    /// Register the "info" subcommand on the given CLI::App.
    void register_info_subcommand(CLI::App &app, int &exit_code);
} // namespace Cli

#endif // YADDNSC_CLI_INFO_H
