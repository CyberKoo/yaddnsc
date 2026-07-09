//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_CORE_LOGGER_H
#define YADDNSC_CORE_LOGGER_H

#include <string_view>
#include "yaddnsc_export.h"

/// Thin logging API exported from the core executable.
///
/// Driver plugins call these through the macros below instead of spdlog
/// directly.  The `_impl` symbols are resolved from the main executable at
/// dlopen time, so plugins never embed spdlog code or carry their own
/// copy of the log state.

namespace CoreLogger {
    YADDNSC_EXPORT void trace_impl(std::string_view msg, const char *file, int line, const char *func);

    YADDNSC_EXPORT void debug_impl(std::string_view msg, const char *file, int line, const char *func);

    YADDNSC_EXPORT void info_impl(std::string_view msg, const char *file, int line, const char *func);

    YADDNSC_EXPORT void warn_impl(std::string_view msg, const char *file, int line, const char *func);

    YADDNSC_EXPORT void error_impl(std::string_view msg, const char *file, int line, const char *func);
}

/// Macros – drop-in replacements for the real spdlog macros.
///
/// Each macro:
///   1. Formats the message using fmt (same engine as spdlog).
///   2. Captures source location (__FILE__, __LINE__, __FUNCTION__).
///   3. Delegates to CoreLogger::xxx_impl, resolved from main executable.
///
/// The output is identical to the original SPDLOG_* macros.
///
/// Usage:
/// @code
///   CORE_LOG_ERROR("Failed to parse API response");
///   CORE_LOG_ERROR("Error: {} ({})", error.message, error.code);
/// @endcode

#define CORE_LOG_TRACE(...)  CoreLogger::trace_impl(fmt::format(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)
#define CORE_LOG_DEBUG(...)  CoreLogger::debug_impl(fmt::format(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)
#define CORE_LOG_INFO(...)   CoreLogger::info_impl(fmt::format(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)
#define CORE_LOG_WARN(...)   CoreLogger::warn_impl(fmt::format(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)
#define CORE_LOG_ERROR(...)  CoreLogger::error_impl(fmt::format(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

#endif //YADDNSC_CORE_LOGGER_H
