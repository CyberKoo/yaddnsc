#ifndef YADDNSC_CONFIG_DIAGNOSTIC_LOCATOR_H
#define YADDNSC_CONFIG_DIAGNOSTIC_LOCATOR_H

#include <cstddef>
#include <string_view>

#include "infrastructure/config/diagnostics/types.h"

namespace Config::Diagnostic {

struct ScanPolicy {
    bool key_offset_after_quote{false};
    bool inspect_complete_string{false};
};

/// Scan the failure prefix and local token lookahead; retain only keys and token facts.
/// Offsets are bytes; returned line/column are 1-based and count bytes after LF.
[[nodiscard]] Site locate(std::string_view buffer, std::size_t offset, ScanPolicy policy = {});

}  // namespace Config::Diagnostic

#endif  // YADDNSC_CONFIG_DIAGNOSTIC_LOCATOR_H
