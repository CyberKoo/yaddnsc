//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for exception classes.
//
// Verifies:
//   - Each exception type can be thrown and caught.
//   - Base type YaddnscException is caught by std::runtime_error.
//   - get_name() returns the correct type name.
//   - Exception-specific accessors work correctly.
// =============================================================================

#include <string_view>
#include <stdexcept>

#include <gtest/gtest.h>

#include "exception/base.h"
#include "exception/driver.h"
#include "exception/bad_driver.h"
#include "exception/config_verification.h"
#include "exception/dns_lookup.h"
#include "exception/socket.h"
#include "dns_error.h"

// ── Base ─────────────────────────────────────────────────────────────────────

TEST(ExceptionTest, YaddnscException_IsRuntimeError) {
    // YaddnscException is abstract (pure virtual get_name()), so we use a
    // concrete subclass to verify the inheritance chain.
    try {
        throw BadDriverException("base error");
    } catch (const std::runtime_error &) {
        SUCCEED();
    } catch (...) {
        FAIL() << "BadDriverException should be caught as std::runtime_error";
    }
}

TEST(ExceptionTest, YaddnscException_What_ReturnsMessage) {
    BadDriverException exc("test message");
    EXPECT_EQ(std::string_view(exc.what()), "test message");
}

// ── DriverException ──────────────────────────────────────────────────────────

TEST(ExceptionTest, DriverException_IsYaddnscException) {
    // DriverException is also abstract; use BadDriverException (a concrete subclass).
    try {
        throw BadDriverException("driver error");
    } catch (const YaddnscException &) {
        SUCCEED();
    } catch (...) {
        FAIL();
    }
}

// ── BadDriverException ───────────────────────────────────────────────────────

TEST(ExceptionTest, BadDriverException_GetName_ReturnsCorrectType) {
    BadDriverException exc("bad driver");
    EXPECT_EQ(exc.get_name(), "BadDriverException");
}

TEST(ExceptionTest, BadDriverException_CatchByYaddnscException) {
    try {
        throw BadDriverException("bad driver");
    } catch (const YaddnscException &) {
        SUCCEED();
    }
}

// ── ConfigVerificationException ──────────────────────────────────────────────

TEST(ExceptionTest, ConfigVerificationException_GetName_ReturnsCorrectType) {
    ConfigVerificationException exc("config invalid");
    EXPECT_EQ(exc.get_name(), "ConfigVerificationException");
}

TEST(ExceptionTest, ConfigVerificationException_IsYaddnscException) {
    try {
        throw ConfigVerificationException("config invalid");
    } catch (const YaddnscException &) {
        SUCCEED();
    }
}

// ── DnsLookupException ───────────────────────────────────────────────────────

TEST(ExceptionTest, DnsLookupException_DefaultConstructor) {
    DnsLookupException exc("dns error");
    EXPECT_EQ(exc.get_name(), "DnsLookupException");
    EXPECT_EQ(exc.get_error(), DNS::Error::UNKNOWN);
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode) {
    DnsLookupException exc("nxdomain", DNS::Error::NX_DOMAIN);
    EXPECT_EQ(exc.get_error(), DNS::Error::NX_DOMAIN);
    EXPECT_EQ(std::string_view(exc.what()), "nxdomain");
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode_Retry) {
    DnsLookupException exc("timeout", DNS::Error::RETRY);
    EXPECT_EQ(exc.get_error(), DNS::Error::RETRY);
}

TEST(ExceptionTest, DnsLookupException_WrapYaddnscException) {
    BadDriverException inner("inner");
    DnsLookupException wrapped(std::move(inner), DNS::Error::CONNECTION);
    EXPECT_EQ(wrapped.get_error(), DNS::Error::CONNECTION);
    EXPECT_EQ(std::string_view(wrapped.what()), "inner");
}

TEST(ExceptionTest, DnsLookupException_WrapConstYaddnscException) {
    const BadDriverException inner("inner");
    DnsLookupException wrapped(inner, DNS::Error::CONFIG);
    EXPECT_EQ(wrapped.get_error(), DNS::Error::CONFIG);
}

// ── SocketException ──────────────────────────────────────────────────────────

TEST(ExceptionTest, SocketException_GetName_ReturnsCorrectType) {
    SocketException exc("socket closed");
    EXPECT_EQ(exc.get_name(), "SocketException");
}

TEST(ExceptionTest, SocketException_WithErrno) {
    // EINVAL = 22 on Linux
    SocketException exc(22, "setsockopt");
    EXPECT_TRUE(exc.has_errno());
    EXPECT_EQ(exc.get_errno(), 22);
    // The message should contain both the context and the system error string
    EXPECT_TRUE(std::string_view(exc.what()).find("setsockopt") != std::string_view::npos);
}

TEST(ExceptionTest, SocketException_WithoutErrno) {
    SocketException exc("EOF");
    EXPECT_FALSE(exc.has_errno());
    EXPECT_EQ(exc.get_errno(), 0);
}

TEST(ExceptionTest, SocketException_WithErrnoEmptyContext) {
    // build_message with empty context — exercises !context.empty() = false branch
    SocketException exc(22, "");
    EXPECT_TRUE(exc.has_errno());
    EXPECT_EQ(exc.get_errno(), 22);
    // The message should contain the system error, but not any custom prefix
    auto msg = std::string_view(exc.what());
    EXPECT_TRUE(msg.find("Invalid argument") != std::string_view::npos ||
                msg.find("Invalid") != std::string_view::npos);
    EXPECT_FALSE(msg.starts_with(":"));
}

TEST(ExceptionTest, SocketException_IsYaddnscException) {
    try {
        throw SocketException("socket error");
    } catch (const YaddnscException &) {
        SUCCEED();
    }
}

// ── Inheritance hierarchy ────────────────────────────────────────────────────

TEST(ExceptionTest, InheritanceHierarchy) {
    // Compile-time check: all concrete exception types inherit from YaddnscException.
    static_assert(std::is_base_of_v<YaddnscException, BadDriverException>);
    static_assert(std::is_base_of_v<YaddnscException, ConfigVerificationException>);
    static_assert(std::is_base_of_v<YaddnscException, DnsLookupException>);
    static_assert(std::is_base_of_v<YaddnscException, SocketException>);
}

TEST(ExceptionTest, AllExceptions_What_IsNonNull) {
    BadDriverException bd("bd");
    ConfigVerificationException cv("cv");
    DnsLookupException dl("dl");
    SocketException sk("sk");

    EXPECT_NE(bd.what(), nullptr);
    EXPECT_NE(cv.what(), nullptr);
    EXPECT_NE(dl.what(), nullptr);
    EXPECT_NE(sk.what(), nullptr);
}
