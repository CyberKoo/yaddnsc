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
#include "domain/network/inet_address.h"

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

    /// `dns resolver` — configured resolver details already formatted by the composition root.
    [[nodiscard]] int present_dns_resolver(bool use_custom_server, std::string_view strategy,
                                           const std::vector<std::string> &servers,
                                           std::string_view legacy_address, unsigned short legacy_port);

    /// `config show` — the parsed configuration as redacted JSON.
    [[nodiscard]] int present_config_show(std::string_view json);

    /// `config test` — validation outcome.
    [[nodiscard]] int present_config_test(const Diagnostics::ConfigTestOutcome &outcome);

    /// `info` — build configuration block.
    [[nodiscard]] int present_info();

    /// Shared catch-all for diagnostic commands: "Error: <what>" on stderr.
    [[nodiscard]] int present_error(const std::exception &e);

} // namespace Cli

#endif // YADDNSC_CLI_PRESENTER_H
