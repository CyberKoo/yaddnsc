//
// Created by Kotarou on 2026/6/30.
//

#include "cli.h"

#include <cstdlib>
#include <iostream>

#include <CLI/CLI.hpp>

#include "version.h"
#include "cli/dns.h"
#include "cli/driver.h"
#include "cli/config.h"
#include "cli/interface.h"


// ===========================================================================
//  CLI11 registration — each subcommand registers its own options
//  ===========================================================================

namespace {

    void
    register_commands(CLI::App &app, std::string &config_path, bool &verbose, bool &run_requested, int &exit_code) {
        app.set_version_flag("--version", yaddnsc::get_full_version(), "Print version information");
        app.add_option("-c,--config", config_path, "Config file path")
                ->default_str("config.json")->check(CLI::ExistingFile)->force_callback();
        app.add_flag("-v,--verbose", verbose, "Enable verbose (debug) logging");

        // run
        {
            auto *run = app.add_subcommand("run", "Run the DDNS client");
            run->alias("r");
            run->callback([&run_requested] { run_requested = true; });
        }

        // Delegate to subcommand-specific registrations.
        cli::register_driver_subcommand(app, config_path, exit_code);
        cli::register_interface_subcommand(app, exit_code);
        cli::register_dns_subcommand(app, config_path, exit_code);
        cli::register_config_subcommand(app, config_path, exit_code);
    }

} // anonymous namespace


// ===========================================================================
//  parse_and_dispatch — parse argv, non-RUN commands handled in callbacks
//  ===========================================================================

cli::CliOutcome cli::parse_and_dispatch(int argc, char *argv[]) {
    std::string config_path = "config.json";
    bool verbose = false;
    bool run_requested = false;
    int exit_code = EXIT_SUCCESS;

    CLI::App app{"Yet another DDNS client"};
    register_commands(app, config_path, verbose, run_requested, exit_code);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return {.exit_code = app.exit(e), .exit_early = true};
    }

    if (!run_requested) {
        // If no subcommand was given at all (e.g. plain "./yaddnsc" with no
        // subcommand and no parse error), print an error and exit with failure.
        if (app.get_subcommands().empty()) {
            std::println(std::cerr, "Error: no subcommand specified. Use --help to see available commands.");
            return {.exit_code = EXIT_FAILURE, .exit_early = true};
        }
        return {.exit_code = exit_code, .exit_early = true};
    }

    return {.should_run = true, .config_path = std::move(config_path), .verbose = verbose};
}
