//
// Unit tests for include/driver/exceptions.h — ParamParseException.
//
// Verifies:
//   - ParamParseException is a concrete DriverException subclass.
//   - get_name() returns "ParamParseException".
//   - Can be caught by DriverException, YaddnscException, and std::runtime_error.
//   - The what() message is preserved.
// =============================================================================

#include <string_view>
#include <stdexcept>

#include <gtest/gtest.h>

#include "driver/exceptions.h"
#include "exception/driver.h"
#include "exception/base.h"

TEST(ParamParseExceptionTest, GetName_ReturnsCorrectType) {
    ParamParseException exc("missing field 'api_key'");
    EXPECT_EQ(exc.get_name(), "ParamParseException");
}

TEST(ParamParseExceptionTest, What_ReturnsMessage) {
    ParamParseException exc("invalid config format");
    EXPECT_EQ(std::string_view(exc.what()), "invalid config format");
}

TEST(ParamParseExceptionTest, IsDriverException) {
    try {
        throw ParamParseException("driver config error");
    } catch (const DriverException &) {
        SUCCEED();
    } catch (...) {
        FAIL() << "ParamParseException should be caught as DriverException";
    }
}

TEST(ParamParseExceptionTest, IsYaddnscException) {
    try {
        throw ParamParseException("yaddnsc error");
    } catch (const YaddnscException &) {
        SUCCEED();
    } catch (...) {
        FAIL() << "ParamParseException should be caught as YaddnscException";
    }
}

TEST(ParamParseExceptionTest, IsStdRuntimeError) {
    try {
        throw ParamParseException("runtime error");
    } catch (const std::runtime_error &) {
        SUCCEED();
    } catch (...) {
        FAIL() << "ParamParseException should be caught as std::runtime_error";
    }
}

TEST(ParamParseExceptionTest, What_IsNonNull) {
    ParamParseException exc("test");
    EXPECT_NE(exc.what(), nullptr);
}

TEST(ParamParseExceptionTest, EmptyMessage_What_IsNonNull) {
    ParamParseException exc("");
    EXPECT_NE(exc.what(), nullptr);
}

TEST(ParamParseExceptionTest, InheritanceCompileTimeCheck) {
    static_assert(std::is_base_of_v<DriverException, ParamParseException>);
    static_assert(std::is_base_of_v<YaddnscException, ParamParseException>);
    static_assert(std::is_base_of_v<std::runtime_error, ParamParseException>);
}
