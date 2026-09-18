//
// Created by Kotarou on 2026/9/17.
//

#include "parser.h"

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/command.h"

#include "version.h"

// ===========================================================================
//  CLI11 registration — pure parsing.
//
//  Each subcommand binds its options directly into a command struct kept
//  alive by shared_ptr (CLI11 defers callback execution until parse), and
//  the callback only moves that struct into the Command variant. No business
//  side effects happen here.
// ===========================================================================

namespace {
/// Add the shared -c,--config option to a subcommand.
template<typename CommandT>
void add_config_option(CLI::App* subcommand, CommandT& command) {
    subcommand->add_option("-c,--config", command.config_path, "Config file path")
        ->default_str("config.json")
        ->check(CLI::ExistingFile);
}

void register_run(CLI::App& app, std::optional<Cli::Command>& command) {
    auto opts = std::make_shared<Cli::RunCommand>();
    auto* run = app.add_subcommand("run", "Run the DDNS client");
    add_config_option(run, *opts);
    run->add_flag("-d,--debug", opts->verbose, "Enable verbose (debug) logging");
    run->callback([&command, opts] { command = *opts; });
}

void register_driver(CLI::App& app, std::optional<Cli::Command>& command) {
    auto* driver = app.add_subcommand("driver", "Manage DDNS driver modules");
    driver->require_subcommand(1);

    auto list_opts = std::make_shared<Cli::DriverListCommand>();
    auto* list = driver->add_subcommand("list", "List all loaded drivers");
    add_config_option(list, *list_opts);
    list->callback([&command, list_opts] { command = *list_opts; });

    auto info_opts = std::make_shared<Cli::DriverInfoCommand>();
    auto* info = driver->add_subcommand("info", "Show detailed information about a driver");
    add_config_option(info, *info_opts);
    info->add_option("name", info_opts->name, "Driver name (e.g. simple, cloudflare)")->required();
    info->callback([&command, info_opts] { command = *info_opts; });
}

void register_interface(CLI::App& app, std::optional<Cli::Command>& command) {
    auto* iface = app.add_subcommand("interface", "Query network interfaces");
    iface->alias("if");
    iface->alias("net");
    iface->require_subcommand(1);

    auto* list = iface->add_subcommand("list", "List all network interfaces");
    list->callback([&command] { command = Cli::InterfaceListCommand{}; });

    auto ip_opts = std::make_shared<Cli::InterfaceIpCommand>();
    auto* ip = iface->add_subcommand("ip", "Show IP addresses of a network interface");
    ip->add_option("name", ip_opts->name, "Interface name (e.g. eth0, en0)")->required();
    ip->callback([&command, ip_opts] { command = *ip_opts; });
}

void register_dns(CLI::App& app, std::optional<Cli::Command>& command) {
    auto* dns = app.add_subcommand("dns", "DNS lookup and diagnostics");
    dns->require_subcommand(1);

    auto resolve_opts = std::make_shared<Cli::DnsResolveCommand>();
    auto* resolve = dns->add_subcommand("resolve", "Resolve a hostname");
    resolve->alias("r");
    add_config_option(resolve, *resolve_opts);
    resolve->add_option("hostname", resolve_opts->host, "Hostname to resolve (e.g. example.com)")->required();
    resolve->add_option("--type", resolve_opts->type, "Record type (A, AAAA, TXT)")
        ->default_str("A")
        ->check(CLI::IsMember(std::vector<std::string>{"A", "AAAA", "TXT"}));
    resolve->callback([&command, resolve_opts] { command = *resolve_opts; });

    auto resolver_opts = std::make_shared<Cli::DnsResolverCommand>();
    auto* resolver = dns->add_subcommand("resolver", "Show configured resolver details");
    add_config_option(resolver, *resolver_opts);
    resolver->callback([&command, resolver_opts] { command = *resolver_opts; });
}

void register_config(CLI::App& app, std::optional<Cli::Command>& command) {
    auto* cfg = app.add_subcommand("config", "Configuration management");
    cfg->require_subcommand(1);

    auto show_opts = std::make_shared<Cli::ConfigShowCommand>();
    auto* show = cfg->add_subcommand("show", "Print resolved configuration as JSON");
    show->alias("s");
    add_config_option(show, *show_opts);
    show->callback([&command, show_opts] { command = *show_opts; });

    auto test_opts = std::make_shared<Cli::ConfigTestCommand>();
    auto* test = cfg->add_subcommand("test", "Validate configuration file and exit");
    test->alias("t");
    test->add_flag("-q,--quiet", test_opts->quiet, "Suppress success message");
    add_config_option(test, *test_opts);
    test->callback([&command, test_opts] { command = *test_opts; });
}

void register_info(CLI::App& app, std::optional<Cli::Command>& command) {
    auto* info = app.add_subcommand("info", "Show build configuration");
    info->callback([&command] { command = Cli::InfoCommand{}; });
}
}  // anonymous namespace

Cli::ParseResult Cli::parse(int argc, char* argv[]) {
    std::optional<Command> command;

    CLI::App app{"Yet another DDNS client"};
    app.set_version_flag("-v,--version", std::string(YADDNSC::get_full_version()), "Print version information");

    register_run(app, command);
    register_driver(app, command);
    register_interface(app, command);
    register_dns(app, command);
    register_config(app, command);
    register_info(app, command);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return {.command = std::nullopt, .exit_code = app.exit(e)};
    }

    if (!command.has_value()) {
        // No leaf subcommand ran. Group-only invocations ("yaddnsc dns")
        // fail earlier via require_subcommand(); reaching here means no
        // subcommand was given at all.
        std::println(std::cerr, "Error: no subcommand specified. Use --help to see available commands.");
        return {.command = std::nullopt, .exit_code = EXIT_FAILURE};
    }

    return {.command = std::move(command), .exit_code = EXIT_SUCCESS};
}
