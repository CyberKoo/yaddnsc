//
// Unit tests for SpdlogLogger (src/infrastructure/logging/spdlog_logger.cpp).
//
// The LogLevel → spdlog::level mapping (to_spdlog_level) is exercised for
// every severity through a recording spdlog sink installed as the default
// logger. Both the source_location overload (log) and the plain-data overload
// (log_explicit, used by plugin Host Services) are checked, plus is_enabled()
// gating against the active spdlog level.
// =============================================================================

#include "infrastructure/logging/spdlog_logger.h"

#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "application/ports/log.h"

namespace {

struct RecordedRecord {
    spdlog::level::level_enum level;
    std::string message;
    std::string file;
    int line;
    std::string function;
};

/// spdlog sink that records every record it receives.
class RecordingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<RecordedRecord> records;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        records.push_back(
            RecordedRecord{msg.level, std::string(msg.payload.data(), msg.payload.size()),
                           msg.source.filename != nullptr ? std::string(msg.source.filename) : std::string{},
                           static_cast<int>(msg.source.line),
                           msg.source.funcname != nullptr ? std::string(msg.source.funcname) : std::string{}});
    }

    void flush_() override {}
};

/// Swap a recording logger in as the spdlog default; restore the previous one
/// on scope exit. Logging is synchronous, so after log() returns the records
/// are stable to read.
class ScopedRecordingLogger {
public:
    ScopedRecordingLogger() : original_(spdlog::default_logger()) {
        auto sink = std::make_shared<RecordingSink>();
        sink_ = sink;
        auto logger = std::make_shared<spdlog::logger>("yaddnsc_spdlog_logger_test", std::move(sink));
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
    }

    ~ScopedRecordingLogger() { spdlog::set_default_logger(original_); }

    ScopedRecordingLogger(const ScopedRecordingLogger&) = delete;
    ScopedRecordingLogger& operator=(const ScopedRecordingLogger&) = delete;

    [[nodiscard]] const std::vector<RecordedRecord>& records() const { return sink_->records; }

private:
    std::shared_ptr<spdlog::logger> original_;
    std::shared_ptr<RecordingSink> sink_;
};

}  // namespace

TEST(SpdlogLoggerTest, ForwardsEveryLevelWithSourceLocation) {
    ScopedRecordingLogger env;
    const SpdlogLogger logger;

    logger.log(LogLevel::trace, "t", std::source_location::current());
    logger.log(LogLevel::debug, "d", std::source_location::current());
    logger.log(LogLevel::info, "i", std::source_location::current());
    logger.log(LogLevel::warn, "w", std::source_location::current());
    logger.log(LogLevel::error, "e", std::source_location::current());
    logger.log(LogLevel::critical, "c", std::source_location::current());

    const auto& records = env.records();
    ASSERT_EQ(records.size(), 6u);
    EXPECT_EQ(records[0].level, spdlog::level::trace);
    EXPECT_EQ(records[1].level, spdlog::level::debug);
    EXPECT_EQ(records[2].level, spdlog::level::info);
    EXPECT_EQ(records[3].level, spdlog::level::warn);
    EXPECT_EQ(records[4].level, spdlog::level::err);
    EXPECT_EQ(records[5].level, spdlog::level::critical);
    for (const auto& record : records) {
        EXPECT_EQ(record.message.size(), 1u);  // single-char payload survives intact
        EXPECT_FALSE(record.file.empty());     // source location forwarded
    }
}

TEST(SpdlogLoggerTest, LogExplicitForwardsExplicitSourceLocation) {
    ScopedRecordingLogger env;
    const SpdlogLogger logger;

    // The plugin Host Services path: location arrives as plain data.
    logger.log_explicit(LogLevel::warn, "via explicit", "plugin.cpp", 42, "update");

    const auto& records = env.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, spdlog::level::warn);
    EXPECT_EQ(records[0].message, "via explicit");
    EXPECT_EQ(records[0].file, "plugin.cpp");
    EXPECT_EQ(records[0].line, 42);
    EXPECT_EQ(records[0].function, "update");
}

TEST(SpdlogLoggerTest, IsEnabledFollowsTheActiveSpdlogLevel) {
    ScopedRecordingLogger env;
    spdlog::default_logger()->set_level(spdlog::level::info);
    const SpdlogLogger logger;

    EXPECT_FALSE(logger.is_enabled(LogLevel::trace));
    EXPECT_FALSE(logger.is_enabled(LogLevel::debug));
    EXPECT_TRUE(logger.is_enabled(LogLevel::info));
    EXPECT_TRUE(logger.is_enabled(LogLevel::warn));
    EXPECT_TRUE(logger.is_enabled(LogLevel::error));
    EXPECT_TRUE(logger.is_enabled(LogLevel::critical));
}
