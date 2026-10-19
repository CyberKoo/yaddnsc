#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_REDACTION_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_REDACTION_H

#include <string>

#include "infrastructure/config/config.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay

namespace Config {

/// Serialize a diagnostic view of a config with sensitive driver fields redacted.
///
/// The file holds API credentials, so this rendering is the only form that may
/// reach the console or a log: URI userinfo in resolver and IP-source addresses
/// and any driver_params member whose key names a token, password, secret or key
/// are replaced before serialization. Nothing else in the config subsystem is a
/// permitted redacting view.
///
/// @param config  Parsed configuration, consumed by value so the caller's copy
///                stays intact.
/// @return        JSON text safe to display.
/// @throws std::runtime_error if the redacted copy cannot be serialized.
[[nodiscard]] std::string redacted_json(AppConfig config);

}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_REDACTION_H
