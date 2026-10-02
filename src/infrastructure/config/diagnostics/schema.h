#ifndef YADDNSC_CONFIG_DIAGNOSTIC_SCHEMA_H
#define YADDNSC_CONFIG_DIAGNOSTIC_SCHEMA_H

#include <string>
#include <vector>

#include "infrastructure/config/diagnostics/types.h"

namespace Config::Diagnostic {

/// Schema facts used by the decision layer; it emits no user-facing prose.
[[nodiscard]] Expectation expectation_for(const Path& path);
[[nodiscard]] std::vector<std::string> member_names_for(const Path& object_path);

}  // namespace Config::Diagnostic

#endif  // YADDNSC_CONFIG_DIAGNOSTIC_SCHEMA_H
