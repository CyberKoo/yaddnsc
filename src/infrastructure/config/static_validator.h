//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CONFIG_STATIC_VALIDATOR_H
#define YADDNSC_CONFIG_STATIC_VALIDATOR_H

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
///
/// @throws std::runtime_error  Only from Uri::parse on a malformed custom
///         resolver address — same escape path as the legacy validator.
[[nodiscard]] auto validate_static(const AppConfig& raw) -> std::vector<domain::ConfigError>;

/// validate_static + normalize: on success returns the runtime
/// configuration, on failure the collected static errors.
/// @throws std::runtime_error  Same Uri::parse escape path as above.
[[nodiscard]] auto validate_and_normalize(const AppConfig& raw)
    -> std::expected<domain::RuntimeConfig, std::vector<domain::ConfigError>>;
}  // namespace Config

#endif  // YADDNSC_CONFIG_STATIC_VALIDATOR_H
