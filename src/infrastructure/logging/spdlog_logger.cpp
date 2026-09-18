//
// Created by Kotarou on 2026/9/17.
//

#include "spdlog_logger.h"

#include <cstdint>
#include <source_location>
#include <string>

#include <spdlog/spdlog.h>

namespace {
[[nodiscard]] constexpr spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE:
            return spdlog::level::trace;
        case LogLevel::DEBUG:
            return spdlog::level::debug;
        case LogLevel::INFO:
            return spdlog::level::info;
        case LogLevel::WARN:
            return spdlog::level::warn;
        case LogLevel::ERROR:
            return spdlog::level::err;
        case LogLevel::CRITICAL:
            return spdlog::level::critical;
    }
    return spdlog::level::info;
}
}  // namespace

bool SpdlogLogger::is_enabled(LogLevel level) const {
    return spdlog::should_log(to_spdlog_level(level));
}

void SpdlogLogger::log(LogLevel level, std::string_view message, const std::source_location& loc) const {
    const spdlog::source_loc spd_loc{loc.file_name(), static_cast<std::int32_t>(loc.line()), loc.function_name()};
    spdlog::default_logger_raw()->log(spd_loc, to_spdlog_level(level),
                                      spdlog::string_view_t(message.data(), message.size()));
}

void SpdlogLogger::log_explicit(LogLevel level,
                                std::string_view message,
                                std::string_view file,
                                int line,
                                std::string_view function) const {
    // spdlog::source_loc stores raw pointers; copy the views so they are
    // guaranteed NUL-terminated for the duration of the synchronous log call.
    const std::string file_str{file};
    const std::string function_str{function};
    const spdlog::source_loc spd_loc{file_str.c_str(), static_cast<std::int32_t>(line), function_str.c_str()};
    spdlog::default_logger_raw()->log(spd_loc, to_spdlog_level(level),
                                      spdlog::string_view_t(message.data(), message.size()));
}
