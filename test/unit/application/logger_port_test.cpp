//
// LoggerPort default behaviour: log_explicit() forwards the record to log()
// using the host location inside the default implementation. The recording
// double inherits that default so the test checks its location as well.
//

#include <source_location>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "application/log.h"
#include "application/ports/log.h"
#include "mocks/null_logger.h"
#include "mocks/recording_logger.h"

TEST(LoggerPortTest, LogExplicit_Default_ReplacesLocationAndPreservesRecord) {
    RecordingLogger logger;

    logger.log_explicit(app::LogLevel::WARN, "via default explicit", "plugin.cpp", 7, "update");

    ASSERT_EQ(logger.records().size(), 1u);
    EXPECT_EQ(logger.records()[0].level, app::LogLevel::WARN);
    EXPECT_EQ(logger.records()[0].message, "via default explicit");
    const auto& record = logger.records()[0];
    EXPECT_TRUE(std::string_view(record.file).ends_with("application/ports/log.h"));
    EXPECT_GT(record.line, 0);
    EXPECT_NE(record.function.find("log_explicit"), std::string::npos);
}

TEST(LoggerPortTest, Ylog_Enabled_FormatsMessage) {
    RecordingLogger logger;
    const auto call_line = std::source_location::current().line() + 1;
    YLOG_WARN(logger, "formatted {}", 42);
    ASSERT_EQ(logger.records().size(), 1u);
    EXPECT_EQ(logger.records()[0].level, app::LogLevel::WARN);
    EXPECT_EQ(logger.records()[0].message, "formatted 42");
    EXPECT_EQ(logger.records()[0].file, std::source_location::current().file_name());
    EXPECT_EQ(logger.records()[0].line, static_cast<int>(call_line));
}

TEST(LoggerPortTest, Ylog_Disabled_DoesNotEvaluateMessageArguments) {
    NullLogger logger;
    bool argument_evaluated = false;

    YLOG_WARN(logger, "formatted {}", (argument_evaluated = true, 42));
    EXPECT_FALSE(argument_evaluated);
}
