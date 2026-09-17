//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_DRIVER_GATEWAY_H
#define YADDNSC_APPLICATION_PORTS_DRIVER_GATEWAY_H

#include <expected>
#include <string>
#include <string_view>

#include "domain/error/error.h"

/// DriverUpdateCommand — everything one driver update call needs, expressed
/// in application terms (no plugin-SDK types cross this boundary).
struct DriverUpdateCommand {
    std::string driver_param; ///< Opaque driver configuration JSON text
    std::string ip_addr;      ///< Resolved IP address to publish
    std::string rd_type;      ///< DNS record type as string (e.g. "A", "AAAA")
    std::string domain;       ///< Parent domain name
    std::string subdomain;    ///< Subdomain label (may be "@" for apex)
    std::string fqdn;         ///< Fully qualified domain name
};

/// DriverGateway — application port for performing one DNS record update
/// through a driver.
///
/// The gateway owns the relationship to the plugin infrastructure: callers
/// never see a Driver* or a module handle, and the gateway guarantees the
/// module outlives every in-flight update.
///
/// Concurrency (Phase 2–3): the temporary adapter behind this port shares one
/// C++ Driver instance across concurrent update() calls — the existing driver
/// contract. Phase 4 replaces the internals with the C ABI (one instance per
/// update) without changing this signature.
class DriverGateway {
public:
    virtual ~DriverGateway() = default;

    /// Perform one update. Failures are reported as DriverError values:
    /// the port never throws for an expected failure (driver not loaded,
    /// upstream rejection, HTTP error, driver exception).
    [[nodiscard]] virtual std::expected<void, domain::DriverError>
    update(std::string_view driver_name, const DriverUpdateCommand &command) const = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_DRIVER_GATEWAY_H
