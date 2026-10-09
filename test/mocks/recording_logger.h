#ifndef YADDNSC_TEST_MOCKS_RECORDING_LOGGER_H
#define YADDNSC_TEST_MOCKS_RECORDING_LOGGER_H

#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "application/ports/log.h"

/// Single-threaded log capture. Uses the port's default log_explicit().
class RecordingLogger : public app::LoggerPort {
public:
    struct Record {
        app::LogLevel level;
        std::string message;
        std::string file;
        int line;
        std::string function;
    };

    [[nodiscard]] bool is_enabled(app::LogLevel) const override { return true; }

    void log(app::LogLevel level, std::string_view message, const std::source_location& loc) const override {
        records_.push_back(
            Record{level, std::string(message), loc.file_name(), static_cast<int>(loc.line()), loc.function_name()});
    }

    [[nodiscard]] const std::vector<Record>& records() const { return records_; }

protected:
    mutable std::vector<Record> records_;
};

/// Plugin contract tests preserve plain-data source locations as well.
class ExplicitRecordingLogger final : public RecordingLogger {
public:
    void log_explicit(app::LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override {
        records_.push_back(Record{level, std::string(message), std::string(file), line, std::string(function)});
    }
};

#endif  // YADDNSC_TEST_MOCKS_RECORDING_LOGGER_H
