#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_ERROR_ADAPTER_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_ERROR_ADAPTER_H

#include <stdint.h>

#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/types.h"

namespace glz {
enum struct error_code : uint32_t;
}  // namespace glz

namespace Config::Diagnostic {

[[nodiscard]] ParseFailure adapt_error(glz::error_code code);
[[nodiscard]] ScanPolicy scan_policy(const ParseFailure& failure) noexcept;

}  // namespace Config::Diagnostic

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_ERROR_ADAPTER_H
