#pragma once

#include "config.h"
#include "domain/config/runtime_config.h"

namespace Config {

/// Maps a raw AppConfig (as parsed from JSON) to the normalized runtime
/// configuration. Pure projection: performs no validation and never throws
/// on content, so commands that consume configuration without verifying it
/// (dns resolve, driver list/info) can use it as well.
///
/// Normalization rules:
///  - Legacy resolver fields (resolver.address/port + use_custom_server) are
///    folded into resolver.servers; servers is left empty when the built-in
///    default resolver should be used.
///  - SubdomainConfig::update_interval carries the effective value
///    (subdomain override if > 0, else the domain-level interval).
///  - driver_param is dumped to opaque JSON text ("{}" when unset).
auto normalize(const AppConfig& raw) -> domain::RuntimeConfig;

} // namespace Config
