//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H
#define YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H

#include <expected>
#include <vector>

#include "domain/error/error.h"

namespace domain {
    struct RuntimeConfig;
}

class DriverCatalogPort;
class NetworkInterfaces;

/// Environment-dependent configuration validation: every referenced driver
/// must be loaded and every referenced network interface must exist on this
/// machine. Static (environment-independent) checks live in
/// Config::validate_static.
///
/// Queries go through ports, so unit tests substitute fakes instead of
/// loading real plugins or enumerating real interfaces.
///
/// Fail-fast: returns the FIRST violated constraint as a single-element
/// error list, with the message wording unchanged from the legacy
/// ConfigValidator.
[[nodiscard]] std::expected<void, std::vector<domain::ConfigError>>
validate_environment(const domain::RuntimeConfig &config, const DriverCatalogPort &catalog,
                     const NetworkInterfaces &interfaces);

#endif // YADDNSC_APPLICATION_ENVIRONMENT_VALIDATOR_H
