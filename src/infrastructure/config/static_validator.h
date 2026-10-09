#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_STATIC_VALIDATOR_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_STATIC_VALIDATOR_H

#include <vector>

#include <expected>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"

namespace Config {
struct AppConfig;

/// Runs every static (environment-independent) configuration check and
/// collects ALL violations as values, in the same order the legacy
/// fail-fast ConfigValidator visited them. Messages are kept verbatim.
///
/// Environment-dependent checks (driver loaded, interface exists) are NOT
/// performed here; they remain in EnvironmentValidator (validator.hpp).
[[nodiscard]] auto validate_static(const AppConfig& raw) -> std::vector<domain::ConfigError>;

/// validate_static + normalize: on success returns the runtime
/// configuration, on failure the collected static errors.
[[nodiscard]] auto validate_and_normalize(const AppConfig& raw)
    -> std::expected<domain::RuntimeConfig, std::vector<domain::ConfigError>>;
}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_STATIC_VALIDATOR_H
