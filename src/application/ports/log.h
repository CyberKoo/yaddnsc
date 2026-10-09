#ifndef YADDNSC_APPLICATION_PORTS_LOG_H
#define YADDNSC_APPLICATION_PORTS_LOG_H

#include <source_location>
#include <string_view>

namespace app {

/// Log severity levels, ordered by verbosity (trace is the most verbose).
enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    CRITICAL,
};

/// LoggerPort — thin, thread-safe log facade port for the application layer and
/// Host Services.
///
/// Every record carries a source location (file / line / function);
/// implementations forward it to the logging backend (spdlog::source_loc for
/// the SpdlogLogger), keeping the `%s` / `%#` pattern fields meaningful.
///
/// Application call sites may use the formatting facade in application/log.h.
/// Adapters consume this contract directly without a formatting dependency.
class LoggerPort {
public:
    virtual ~LoggerPort() = default;

    /// Whether `level` would be emitted — allows callers to skip unnecessary work.
    [[nodiscard]] virtual bool is_enabled(LogLevel level) const = 0;

    /// Emit one already-formatted record.
    /// @note Implementations must be thread-safe.
    virtual void log(LogLevel level, std::string_view message, const std::source_location& loc) const = 0;

    /// Emit one record whose call site lies outside this process image (a
    /// driver plugin logging through Host Services): the location arrives as
    /// plain data because std::source_location cannot be synthesised.
    /// The default implementation replaces the supplied location with the
    /// host call site inside LoggerPort::log_explicit().
    virtual void log_explicit(LogLevel level, std::string_view message, [[maybe_unused]] std::string_view file,
                              [[maybe_unused]] int line, [[maybe_unused]] std::string_view function) const {
        log(level, message, std::source_location::current());
    }
};

}  // namespace app


#endif  // YADDNSC_APPLICATION_PORTS_LOG_H
