#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_PARSE_DIAGNOSTIC_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_PARSE_DIAGNOSTIC_H

#include <stdint.h>
#include <cstddef>
#include <string>
#include <string_view>

namespace glz {
enum struct error_code : uint32_t;
}  // namespace glz

namespace Config::Diagnostic {

/// Describe a failed JSON read of the application configuration.
///
/// The result names the failing position, the complete member/array-element
/// path, and what that location accepts. Lines and columns are 1-based; columns
/// count bytes since the last LF, not Unicode characters or display cells.
/// File-operation errors name the configuration path without a content position.
/// The scanner stops at the failure, with local lookahead for key/token details. It never carries a configuration
/// *value*: only key names, JSON kinds, and expectations taken from the Config schema, because the file holds API
/// credentials and the message reaches logs.
///
/// @param config_path   Path named in the message.
/// @param buffer        Raw file contents, inspected for positions, key names,
///                      and token kinds; values are never included in the result.
/// @param error_offset  Byte offset reported by the failed read.
/// @param code          Error code reported by the failed read.
/// @return              Message body, for example
///   `config file "a.json" (line 4, column 5): unknown key "driver_dire" in
///    "drivers" — did you mean "driver_dir"?`
[[nodiscard]] std::string describe_parse_error(const std::string& config_path, std::string_view buffer,
                                               std::size_t error_offset, glz::error_code code);

}  // namespace Config::Diagnostic

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_PARSE_DIAGNOSTIC_H
