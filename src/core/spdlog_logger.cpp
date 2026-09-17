//
// Created by Kotarou on 2026/9/17.
//

#include "spdlog_logger.h"

#include <spdlog/spdlog.h>

namespace {
    [[nodiscard]] constexpr spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::debug:
                return spdlog::level::debug;
            case LogLevel::info:
                return spdlog::level::info;
            case LogLevel::warn:
                return spdlog::level::warn;
            case LogLevel::error:
                return spdlog::level::err;
            case LogLevel::critical:
                return spdlog::level::critical;
        }
        return spdlog::level::info;
    }
} // namespace

bool SpdlogLogger::is_enabled(LogLevel level) const {
    return spdlog::should_log(to_spdlog_level(level));
}

void SpdlogLogger::log(LogLevel level, std::string_view message, const std::source_location &loc) const {
    const spdlog::source_loc spd_loc{loc.file_name(), static_cast<std::int32_t>(loc.line()), loc.function_name()};
    spdlog::default_logger_raw()->log(spd_loc, to_spdlog_level(level), spdlog::string_view_t(message.data(), message.size()));
}
