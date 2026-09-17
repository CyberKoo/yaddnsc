//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_DIAGNOSTICS_H
#define YADDNSC_APPLICATION_DIAGNOSTICS_H

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/driver_catalog.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"

class DnsResolverPort;
class NetworkInterfaces;

/// Diagnostic command handlers — thin application functions over the ports.
///
/// Each function returns a result object; nothing here prints. The CLI
/// presenter maps result objects to text and exit codes. These are free
/// functions on purpose: per-command service classes would be empty shells.
namespace Diagnostics {

    /// One row of `driver list`: the loaded name plus either its descriptor
    /// or the error text from the failed descriptor query.
    struct DriverListItem {
        std::string name;
        std::optional<DriverDescription> detail; ///< nullopt when the query failed
        std::string error;                       ///< failure text when detail is nullopt
    };

    /// List every loaded driver with its description (per-driver failures are
    /// captured into the item, mirroring the legacy output shape).
    [[nodiscard]] std::vector<DriverListItem> list_drivers(const DriverCatalogPort &catalog);

    /// One row of `interface list`: an interface name and its addresses.
    struct InterfaceListItem {
        std::string name;
        std::vector<InetAddress> addresses;
    };

    /// List every interface with its addresses.
    /// @throws std::runtime_error  If an interface disappears mid-listing
    ///         (legacy behaviour: the command aborts with "Error: ...").
    [[nodiscard]] std::vector<InterfaceListItem> list_interfaces(const NetworkInterfaces &interfaces);

    /// Outcome of one `dns resolve` command.
    struct DnsResolveOutcome {
        std::string host;      ///< Hostname as provided on the command line
        std::string type_text; ///< Record type as provided on the command line
        /// The lookup result; nullopt when `type_text` is not a known record
        /// type (the presenter prints the "unknown record type" error).
        std::optional<std::expected<std::vector<std::string>, DnsErrorInfo>> lookup;
    };

    /// Resolve `host` through the resolver port. The record type string is
    /// matched case-insensitively (legacy behaviour for direct invocations;
    /// the CLI parser already restricts --type to A/AAAA/TXT).
    [[nodiscard]] DnsResolveOutcome dns_resolve(const DnsResolverPort &resolver, std::string host,
                                                std::string type_text);

    /// Error of a `config test` run; `kind` selects the legacy message prefix.
    struct ConfigTestError {
        enum class Kind {
            VERIFICATION, ///< "Configuration verification failed: ..."
            FATAL,        ///< "Fatal error: unrecoverable exception: ..."
            GENERIC,      ///< "Failed to validate configuration: ..."
        };
        Kind kind;
        std::string message;
    };

    /// Outcome of a `config test` run: nullopt error means the test passed.
    struct ConfigTestOutcome {
        bool quiet;                        ///< -q/--quiet: no stdout on success
        std::optional<ConfigTestError> error;
    };

} // namespace Diagnostics

#endif // YADDNSC_APPLICATION_DIAGNOSTICS_H
