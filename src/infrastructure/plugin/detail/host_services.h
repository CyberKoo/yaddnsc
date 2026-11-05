#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_HOST_SERVICES_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_HOST_SERVICES_H

#include <cstdint>
#include <optional>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "infrastructure/http/types.h"

namespace plugin::detail {

/// Map ABI values without throwing. Unknown levels use INFO; unknown methods
/// are rejected rather than silently selecting a verb.
[[nodiscard]] app::LogLevel to_log_level(yaddnsc_log_level level) noexcept;
[[nodiscard]] std::optional<http::Method> to_http_method(yaddnsc_http_method method) noexcept;

/// Write only within the caller's known error prefix, without allocating.
/// Message storage is borrowed: it stays valid until the next ABI entry or
/// service callback on this thread, context destruction, or module unload.
void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                 uint32_t retry_after_seconds = 0) noexcept;

/// Forward a borrowed log record. The owning C callback contains exceptions;
/// malformed source locations are tolerated and disabled levels do no work.
void log_record(const app::LoggerPort& logger, yaddnsc_log_level level, yaddnsc_string message,
                const yaddnsc_source_location* location);

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_HOST_SERVICES_H
