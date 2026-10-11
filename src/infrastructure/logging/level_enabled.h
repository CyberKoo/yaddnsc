#ifndef YADDNSC_INFRASTRUCTURE_LOGGING_LEVEL_ENABLED_H
#define YADDNSC_INFRASTRUCTURE_LOGGING_LEVEL_ENABLED_H

#include <spdlog/spdlog.h>

namespace logging {

/// Whether the process-wide default logger would emit `level`.
///
/// A guard for arguments that are expensive to build or have side effects —
/// draining the OpenSSL error queue, formatting a list — so the work happens
/// only when the record would be written. The SPDLOG_* macros expand their
/// arguments before the level check, so they cannot do this themselves.
///
/// Null after logging::shutdown(): there is nowhere left to write, so every
/// level reports disabled. Thread-safe (the default logger is).
[[nodiscard]] inline bool level_enabled(spdlog::level::level_enum level) noexcept {
    const auto* const logger = spdlog::default_logger_raw();
    return logger != nullptr && logger->should_log(level);
}

}  // namespace logging

#endif  // YADDNSC_INFRASTRUCTURE_LOGGING_LEVEL_ENABLED_H
