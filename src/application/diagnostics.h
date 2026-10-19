#ifndef YADDNSC_APPLICATION_DIAGNOSTICS_H
#define YADDNSC_APPLICATION_DIAGNOSTICS_H

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/driver_catalog.h"
#include "coro/task.hpp"
#include "coro/time.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"

/// Diagnostic command handlers — thin application functions over the ports.
///
/// Each function returns a result object; nothing here prints. The CLI
/// presenter maps result objects to text and exit codes. These are free
/// functions on purpose: per-command service classes would be empty shells.
namespace app {

class NetworkInterfacesPort;
class ResolverPort;

/// One row of `driver list`: the loaded name plus either its descriptor
/// or the structured error from the failed descriptor query.
struct DriverListItem {
    std::string name;
    std::expected<DriverDescription, domain::DriverError> detail;
};

/// List every loaded driver with its description. Missing drivers are captured
/// into the item; allocation failures and other unexpected exceptions propagate.
[[nodiscard]] std::vector<DriverListItem> list_drivers(const DriverCatalogPort& catalog);

/// One row of `interface list`: an interface name and its addresses.
struct InterfaceListItem {
    std::string name;
    std::vector<domain::InetAddress> addresses;
};

/// List every interface with its addresses. Rows come from a single
/// enumeration pass (NetworkInterfacesPort::list()), so an interface that
/// disappears mid-listing simply has no row.
[[nodiscard]] std::vector<InterfaceListItem> list_interfaces(const NetworkInterfacesPort& interfaces);

/// Outcome of one `dns resolve` command.
struct DnsResolveOutcome {
    std::string host;       ///< Hostname as provided on the command line
    std::string type_text;  ///< Record type as provided on the command line
    /// The lookup result; nullopt when `type_text` is not a known record
    /// type (the presenter prints the "unknown record type" error).
    std::optional<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> lookup;
};

/// Coroutine `dns resolve`: match the record type string case-insensitively
/// (for direct invocations; the CLI parser already restricts --type to
/// A/AAAA/TXT) and look the name up through the resolver port.
///
/// A nullopt `lookup` means `type_text` was not a known record kind. Failure:
/// the lookup's DnsErrorInfo value; a defect (allocation) propagates.
[[nodiscard]] coro::Task<DnsResolveOutcome> dns_resolve(ResolverPort& resolver, std::string host,
                                                        std::string type_text);

/// Bounded one-shot lookup. Own timeout becomes a CONNECTION error; ancestor
/// cancellation and defects propagate. No signal watcher is installed here.
/// The resolver must outlive the task; call on the loop thread.
[[nodiscard]] coro::Task<DnsResolveOutcome> dns_resolve_command(ResolverPort& resolver, std::string host,
                                                                std::string type_text, coro::Duration budget);

}  // namespace app

#endif  // YADDNSC_APPLICATION_DIAGNOSTICS_H
