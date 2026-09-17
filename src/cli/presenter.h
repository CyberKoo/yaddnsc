//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CLI_PRESENTER_H
#define YADDNSC_CLI_PRESENTER_H

#include <exception>
#include <string>
#include <vector>

#include "application/diagnostics.h"
#include "application/ports/driver_catalog.h"
#include "config/config.h"
#include "network/inet_address.h"

/// CLI presenter — maps command result objects to stdout/stderr text and
/// exit codes. All user-visible wording lives here (single place), including
/// the `config show` sensitive-field redaction rule. Nothing in this file
/// performs business I/O.
namespace Cli {

    /// `driver list` — summary of every loaded driver.
    [[nodiscard]] int present_driver_list(const std::vector<Diagnostics::DriverListItem> &items);

    /// `driver info` — detail block for one driver.
    [[nodiscard]] int present_driver_info(const DriverDescription &detail);

    /// `interface list` — all interfaces with their addresses.
    [[nodiscard]] int present_interface_list(const std::vector<Diagnostics::InterfaceListItem> &items);

    /// `interface ip` — addresses of one interface.
    [[nodiscard]] int present_interface_ip(const std::string &name, const std::vector<InetAddress> &addresses);

    /// `dns resolve` — lookup outcome (all lookup results exit SUCCESS; an
    /// unknown record type exits FAILURE).
    [[nodiscard]] int present_dns_resolve(const Diagnostics::DnsResolveOutcome &outcome);

    /// `dns resolver` — configured resolver details.
    [[nodiscard]] int present_dns_resolver(const Config::ResolverConfig &resolver);

    /// `config show` — the parsed configuration as JSON, with sensitive
    /// driver_param fields redacted (rule lives here, applied nowhere else).
    [[nodiscard]] int present_config_show(Config::AppConfig config);

    /// `config test` — validation outcome.
    [[nodiscard]] int present_config_test(const Diagnostics::ConfigTestOutcome &outcome);

    /// `info` — build configuration block.
    [[nodiscard]] int present_info();

    /// Shared catch-all for diagnostic commands: "Error: <what>" on stderr.
    [[nodiscard]] int present_error(const std::exception &e);

} // namespace Cli

#endif // YADDNSC_CLI_PRESENTER_H
