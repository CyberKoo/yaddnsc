#include "spdlog_logger.h"

#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace {
[[nodiscard]] constexpr spdlog::level::level_enum to_spdlog_level(app::LogLevel level) noexcept {
    switch (level) {
        case app::LogLevel::TRACE:
            return spdlog::level::trace;
        case app::LogLevel::DEBUG:
            return spdlog::level::debug;
        case app::LogLevel::INFO:
            return spdlog::level::info;
        case app::LogLevel::WARN:
            return spdlog::level::warn;
        case app::LogLevel::ERROR:
            return spdlog::level::err;
        case app::LogLevel::CRITICAL:
            return spdlog::level::critical;
    }
    return spdlog::level::info;
}

/// Intern a plugin-supplied file/function name so the async drain thread can
/// still format `%s`/`%#` after the caller's views are gone.
///
/// spdlog's async path copies the source_loc *pointers* into the queued message,
/// not the strings, while a plugin's location data is only valid for the
/// duration of the host-services log call. Interning keeps the pointers valid
/// for the process lifetime; the number of distinct plugin source locations is
/// bounded, and unordered_set nodes are stable across rehashing.
[[nodiscard]] const char* intern_source_string(const std::string_view value) {
    static std::mutex mutex;
    static std::unordered_set<std::string> pool;
    const std::lock_guard lock(mutex);
    return pool.emplace(value).first->c_str();
}
}  // namespace

bool SpdlogLogger::is_enabled(app::LogLevel level) const {
    return spdlog::should_log(to_spdlog_level(level));
}

void SpdlogLogger::log(app::LogLevel level, std::string_view message, const std::source_location& loc) const {
    const spdlog::source_loc spd_loc{loc.file_name(), static_cast<std::int32_t>(loc.line()), loc.function_name()};
    spdlog::default_logger_raw()->log(spd_loc, to_spdlog_level(level),
                                      spdlog::string_view_t(message.data(), message.size()));
}

void SpdlogLogger::log_explicit(app::LogLevel level, std::string_view message, std::string_view file, int line,
                                std::string_view function) const {
    // Intern the location strings: an async logger queues a copy of the
    // source_loc pointers and formats on its drain thread, which would outlive
    // these plugin-supplied views.
    const spdlog::source_loc spd_loc{intern_source_string(file), static_cast<std::int32_t>(line),
                                     intern_source_string(function)};
    spdlog::default_logger_raw()->log(spd_loc, to_spdlog_level(level),
                                      spdlog::string_view_t(message.data(), message.size()));
}
