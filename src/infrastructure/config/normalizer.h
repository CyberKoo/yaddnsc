#pragma once

#include "domain/config/runtime_config.h"

namespace Config {
struct AppConfig;

/// Maps a raw AppConfig (as parsed from JSON) to the normalized runtime
/// configuration. Performs no validation, so callers that enter the runtime
/// graph must use validate_and_normalize() first.
///
/// Normalization rules:
///  - Legacy resolver fields (resolver.address/port + use_custom_server) are
///    folded into resolver.servers; disabled custom DNS is materialized as
///    the configured built-in default server.
///  - SubdomainConfig::update_interval carries the effective value
///    (subdomain override if > 0, else the domain-level interval).
///  - driver_param is dumped to opaque JSON text ("{}" when unset).
///
/// A missing subdomain record type falls back to A with a warning (never an
/// error) so legacy configs that rely on the default keep working.
auto normalize(const AppConfig& raw) -> domain::RuntimeConfig;

}  // namespace Config
