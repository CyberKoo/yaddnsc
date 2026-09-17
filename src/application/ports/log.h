//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_LOG_H
#define YADDNSC_APPLICATION_PORTS_LOG_H

#include <source_location>
#include <string_view>

#include "support/fmt.hpp"

/// Log severity levels, ordered by verbosity (trace is the most verbose).
enum class LogLevel {
    trace,
    debug,
    info,
    warn,
    error,
    critical,
};

/// Logger — thin, thread-safe log facade port for the application layer and
/// Host Services.
///
/// Every record carries a source location (file / line / function);
/// implementations forward it to the logging backend (spdlog::source_loc for
/// the SpdlogLogger), keeping the `%s` / `%#` pattern fields meaningful.
///
/// Callers should use the YLOG_* macros below: they capture the call site via
/// std::source_location::current() and skip message formatting entirely when
/// the level is disabled.
class Logger {
public:
    virtual ~Logger() = default;

    /// Whether `level` would be emitted — used by the macros to skip work.
    [[nodiscard]] virtual bool is_enabled(LogLevel level) const = 0;

    /// Emit one already-formatted record.
    /// @note Implementations must be thread-safe.
    virtual void log(LogLevel level, std::string_view message, const std::source_location &loc) const = 0;

    /// Emit one record whose call site lies outside this process image (a
    /// driver plugin logging through Host Services): the location arrives as
    /// plain data because std::source_location cannot be synthesised.
    /// The default implementation drops the explicit location.
    virtual void log_explicit(LogLevel level, std::string_view message, std::string_view file, int line,
                              std::string_view function) const {
        (void)file;
        (void)line;
        (void)function;
        log(level, message, std::source_location::current());
    }
};

/// Log a formatted message at the given level through a Logger.
/// The format string and arguments are only evaluated when the level is
/// enabled; the source location is captured at the call site.
#define YLOG(logger, level, ...) \
    do { \
        if ((logger).is_enabled(level)) { \
            (logger).log(level, fmt::format(__VA_ARGS__), std::source_location::current()); \
        } \
    } while (0)

#define YLOG_DEBUG(logger, ...) YLOG(logger, LogLevel::debug, __VA_ARGS__)
#define YLOG_INFO(logger, ...) YLOG(logger, LogLevel::info, __VA_ARGS__)
#define YLOG_WARN(logger, ...) YLOG(logger, LogLevel::warn, __VA_ARGS__)
#define YLOG_ERROR(logger, ...) YLOG(logger, LogLevel::error, __VA_ARGS__)
#define YLOG_CRITICAL(logger, ...) YLOG(logger, LogLevel::critical, __VA_ARGS__)

#endif // YADDNSC_APPLICATION_PORTS_LOG_H
