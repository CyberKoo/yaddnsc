#ifndef YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H
#define YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H

#include <vector>

#include <expected>

#include "domain/error/error.h"

namespace domain {
struct RuntimeConfig;
}

namespace app {

class DriverCatalogPort;
class NetworkInterfacesPort;

/// Environment-dependent configuration validation: every referenced driver
/// must be loaded and every referenced network interface must exist on this
/// machine. Static (environment-independent) checks live in
/// Config::validate_static.
///
/// Queries go through ports, so unit tests substitute fakes instead of
/// loading real plugins or enumerating real interfaces.
///
/// Also rejects a record type the referenced driver has no capability for,
/// which the host would otherwise refuse on every single update cycle.
///
/// Collect-all: every violated constraint is reported, in configuration order,
/// so one startup surfaces one broken configuration instead of one mistake per
/// restart. An empty error list means the configuration is usable here.
/// Message wording is unchanged from the legacy ConfigValidator.
[[nodiscard]] std::expected<void, std::vector<domain::ConfigError>> validate_environment(
    const domain::RuntimeConfig& config, const DriverCatalogPort& catalog, const NetworkInterfacesPort& interfaces);

}  // namespace app

#endif  // YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H
