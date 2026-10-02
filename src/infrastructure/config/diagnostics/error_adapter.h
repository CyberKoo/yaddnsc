#ifndef YADDNSC_CONFIG_DIAGNOSTIC_ERROR_ADAPTER_H
#define YADDNSC_CONFIG_DIAGNOSTIC_ERROR_ADAPTER_H

#include <glaze/core/context.hpp>

#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/types.h"

namespace Config::Diagnostic {

[[nodiscard]] ParseFailure adapt_error(glz::error_code code);
[[nodiscard]] ScanPolicy scan_policy(const ParseFailure& failure) noexcept;

}  // namespace Config::Diagnostic

#endif  // YADDNSC_CONFIG_DIAGNOSTIC_ERROR_ADAPTER_H
