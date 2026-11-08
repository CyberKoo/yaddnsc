#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_NORMALIZER_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_NORMALIZER_H

#include <string_view>

#include "domain/config/runtime_config.h"

namespace Config {
struct AppConfig;
struct DriverConfig;
struct ResolverConfig;

/// Normalize only the driver or resolver slices for diagnostic commands.
[[nodiscard]] auto normalize_driver_settings(const DriverConfig& raw) -> domain::DriverSettings;
[[nodiscard]] auto normalize_resolver_settings(const ResolverConfig& raw, std::string_view bootstrap_dns)
    -> domain::ResolverSettings;

/// Maps a raw AppConfig (as parsed from JSON) to the normalized runtime
/// configuration. Performs no validation: the full runtime graph must use
/// validate_and_normalize() first, while diagnostic slices use their own checks.
///
/// Normalization rules:
///  - Custom resolver servers are copied into the runtime settings; disabled
///    custom DNS is materialized as the configured built-in default server.
///  - SubdomainConfig::update_interval carries the effective value
///    (subdomain override if > 0, else the domain-level interval).
///  - driver_params is dumped to opaque JSON text ("{}" when unset).
///
/// A missing subdomain record type falls back to A with a warning (never an
/// error) so configs that rely on the default keep working.
auto normalize(const AppConfig& raw) -> domain::RuntimeConfig;

}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_NORMALIZER_H
