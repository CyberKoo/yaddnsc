//
// NullLogger — Logger test double that drops every record.
//
// is_enabled() is always false, so the YLOG_* macros skip message
// formatting entirely (same fast path as a disabled spdlog level).
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_NULL_LOGGER_H
#define YADDNSC_TEST_MOCKS_NULL_LOGGER_H

#include <source_location>
#include <string_view>

#include "application/ports/log.h"

class NullLogger final : public Logger {
public:
    [[nodiscard]] bool is_enabled(LogLevel) const override { return false; }

    void log(LogLevel, std::string_view, const std::source_location &) const override {}
};

#endif // YADDNSC_TEST_MOCKS_NULL_LOGGER_H
