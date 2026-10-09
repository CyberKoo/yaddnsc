//
// app — application ports for updates and diagnostic commands.
//
// ResolverPort, IpSourcePort and GatewayPort expose coroutine operations:
// cancellation comes from the awaiting scope and operational outcomes are
// values. ResolverPort also serves one-shot DNS diagnostics; GatewayPort also
// serves config test validation, outside the update-loop lifecycle.
//
// The synchronous ports live under application/ports/: DriverCatalogPort and
// NetworkInterfacesPort serve environment validation and CLI diagnostics;
// LoggerPort receives application and Host Services log records.
// Concrete adapters are assembled in composition and live in infrastructure.
//

#ifndef YADDNSC_APPLICATION_PORTS_H
#define YADDNSC_APPLICATION_PORTS_H

#include <string>
#include <vector>

#include <expected>

#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// ResolverPort — look up the current DNS records for a name.
///
/// Failure: DnsErrorInfo values; cancellation surfaces as `coro::Cancelled`
/// (a definitive value), and the await is a scope checkpoint.
/// Thread safety: an implementation may hold session state; call from the loop
/// thread only.
class ResolverPort {
public:
    virtual ~ResolverPort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string host, domain::RecordKind type) = 0;
};

/// IpSourcePort — obtain local address candidates for a subdomain.
///
/// Failure: IpSourceError values (cancellation is `coro::Cancelled`).
/// An empty candidate list is a success.
class IpSourcePort {
public:
    virtual ~IpSourcePort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) = 0;
};

/// GatewayPort — driver updates and one-shot configuration diagnostics.
///
/// Failure: DriverError values. Cancellation: the await is an abandon point —
/// a driver cycle that already started runs to completion while the result is
/// `coro::Cancelled`; a cycle not yet started never enters the driver.
class GatewayPort {
public:
    virtual ~GatewayPort() = default;

    /// Diagnostic path for `config test`, outside the update-loop lifecycle.
    /// Validate driver parameters without updating. A missing validation entry
    /// returns DriverError; cancellation propagates. Loop-thread only.
    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) = 0;

    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_H
