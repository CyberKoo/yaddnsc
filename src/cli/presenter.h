#ifndef YADDNSC_CLI_PRESENTER_H
#define YADDNSC_CLI_PRESENTER_H

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

namespace app {
struct ConfigTestOutcome;
struct DnsResolveOutcome;
struct DriverListItem;
struct InterfaceListItem;
struct DriverDescription;
}  // namespace app

namespace domain {
class InetAddress;
struct DriverError;
struct DnsServer;
}  // namespace domain

/// CLI presenter — maps command result objects to stdout/stderr text and
/// exit codes. All user-visible wording lives here (single place), including
/// the `config show` sensitive-field redaction rule. Nothing in this file
/// performs business I/O.
namespace Cli {

/// `driver list` — summary of every loaded driver.
[[nodiscard]] int present_driver_list(const std::vector<app::DriverListItem>& items);

/// `driver info` — detail block for one driver.
[[nodiscard]] int present_driver_info(std::string_view name,
                                      const std::expected<app::DriverDescription, domain::DriverError>& result);

/// `interface list` — all interfaces with their addresses.
[[nodiscard]] int present_interface_list(const std::vector<app::InterfaceListItem>& items);

/// `interface ip` — addresses of one interface; a missing interface prints
/// the legacy "Error: Interface <name> not found" line and exits FAILURE.
[[nodiscard]] int present_interface_ip(const std::string& name,
                                       const std::optional<std::vector<domain::InetAddress>>& addresses);

/// `dns resolve` — lookup outcome (all lookup results exit SUCCESS; an
/// unknown record type exits FAILURE).
[[nodiscard]] int present_dns_resolve(const app::DnsResolveOutcome& outcome);

/// `dns resolver` — configured resolver details; endpoint formatting is owned here.
[[nodiscard]] int present_dns_resolver(bool use_custom_servers, std::string_view strategy,
                                       const std::vector<domain::DnsServer>& servers);

/// `config show` — the parsed configuration as redacted JSON.
[[nodiscard]] int present_config_show(std::string_view json);

/// `config test` — validation outcome.
[[nodiscard]] int present_config_test(const app::ConfigTestOutcome& outcome);

/// `info` — build configuration block.
[[nodiscard]] int present_info();

/// Shared catch-all for diagnostic commands: "Error: <what>" on stderr.
[[nodiscard]] int present_error(const std::exception& e);

}  // namespace Cli

#endif  // YADDNSC_CLI_PRESENTER_H
