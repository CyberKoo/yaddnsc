//
// Unit tests for the Logger port default behaviour (src/application/ports/log.h).
//
// The port declares two virtual overloads: log() (source_location captured at
// the call site) and log_explicit() (plain-data location from a driver plugin
// logging through Host Services). The default log_explicit() drops the
// explicit location and forwards to log() with a synthesized one — a Logger
// implementation that does not override it (e.g. a minimal test double, or a
// plugin-host logger that only implements log()) must still emit the record.
// =============================================================================

#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "application/ports/log.h"

namespace {

struct RecordedRecord {
    LogLevel level;
    std::string message;
};

/// Records every record. Deliberately does NOT override log_explicit() so the
/// base-class default (forward to log() with a synthesized location) runs.
class RecordingLogger final : public Logger {
public:
    [[nodiscard]] bool is_enabled(LogLevel) const override { return true; }

    void log(LogLevel level, std::string_view message, const std::source_location &) const override {
        records_.push_back(RecordedRecord{level, std::string(message)});
    }

    [[nodiscard]] const std::vector<RecordedRecord> &records() const { return records_; }

private:
    mutable std::vector<RecordedRecord> records_;
};

} // namespace

TEST(LoggerPortTest, DefaultLogExplicitForwardsLevelAndMessageToLog) {
    RecordingLogger logger;

    logger.log_explicit(LogLevel::warn, "via default explicit", "plugin.cpp", 7, "update");

    ASSERT_EQ(logger.records().size(), 1u);
    EXPECT_EQ(logger.records()[0].level, LogLevel::warn);
    EXPECT_EQ(logger.records()[0].message, "via default explicit");
}

TEST(LoggerPortTest, IsEnabledDrivesYlogMacroFormatting) {
    RecordingLogger logger;
    // The level is enabled, so the YLOG_* macro formats and emits...
    YLOG_WARN(logger, "formatted {}", 42);
    ASSERT_EQ(logger.records().size(), 1u);
    EXPECT_EQ(logger.records()[0].level, LogLevel::warn);
    EXPECT_EQ(logger.records()[0].message, "formatted 42");
}
