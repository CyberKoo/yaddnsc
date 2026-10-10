#ifndef YADDNSC_APPLICATION_LOG_MACROS_H
#define YADDNSC_APPLICATION_LOG_MACROS_H

#include <source_location>          // IWYU pragma: keep — used in the YLOG macro body

#include "application/ports/log.h"  // IWYU pragma: keep — LoggerPort is the YLOG macro's parameter type
#include "support/fmt.hpp"          // IWYU pragma: keep — fmt::format in the YLOG macro body

/// Format a message only when its level is enabled, capturing the call site.
/// The logger expression must identify the same logger on each evaluation.
#define YLOG(logger, level, ...)                                                            \
    do {                                                                                    \
        if ((logger).is_enabled(level)) {                                                   \
            (logger).log(level, fmt::format(__VA_ARGS__), std::source_location::current()); \
        }                                                                                   \
    } while (0)

#define YLOG_DEBUG(logger, ...) YLOG(logger, app::LogLevel::DEBUG, __VA_ARGS__)
#define YLOG_INFO(logger, ...) YLOG(logger, app::LogLevel::INFO, __VA_ARGS__)
#define YLOG_WARN(logger, ...) YLOG(logger, app::LogLevel::WARN, __VA_ARGS__)
#define YLOG_ERROR(logger, ...) YLOG(logger, app::LogLevel::ERROR, __VA_ARGS__)
#define YLOG_CRITICAL(logger, ...) YLOG(logger, app::LogLevel::CRITICAL, __VA_ARGS__)

#endif  // YADDNSC_APPLICATION_LOG_MACROS_H
