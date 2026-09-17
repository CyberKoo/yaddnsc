//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_CORE_SPDLOG_LOGGER_H
#define YADDNSC_CORE_SPDLOG_LOGGER_H

#include <source_location>
#include <string_view>

#include "application/ports/log.h"

/// SpdlogLogger — Logger port implementation on top of the process-wide
/// spdlog default logger.
///
/// Forwards the source location to spdlog::source_loc, so log patterns using
/// %s / %# keep working. Thread-safe (spdlog's default logger is).
class SpdlogLogger final : public Logger {
public:
    [[nodiscard]] bool is_enabled(LogLevel level) const override;

    void log(LogLevel level, std::string_view message, const std::source_location &loc) const override;

    void log_explicit(LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override;
};

#endif // YADDNSC_CORE_SPDLOG_LOGGER_H
