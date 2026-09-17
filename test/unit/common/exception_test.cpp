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

#include "support/exception.h"
#include "infrastructure/plugin/plugin_load_exception.h"
#include "infrastructure/config/config_verification_exception.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/network/socket_exception.h"
#include "domain/error/dns_error.h"

// ── Base ─────────────────────────────────────────────────────────────────────

TEST(ExceptionTest, YaddnscException_IsRuntimeError) {
    // YaddnscException is abstract (pure virtual get_name()), so we use a
    // concrete subclass to verify the inheritance chain.
    try {
        throw PluginLoadException("base error");
    } catch (const std::runtime_error &) {
        SUCCEED();
    } catch (...) {
        FAIL() << "PluginLoadException should be caught as std::runtime_error";
    }
}

TEST(ExceptionTest, YaddnscException_What_ReturnsMessage) {
    PluginLoadException exc("test message");
    EXPECT_EQ(std::string_view(exc.what()), "test message");
}

// ── PluginLoadException ───────────────────────────────────────────────────────

TEST(ExceptionTest, PluginLoadException_GetName_ReturnsCorrectType) {
    PluginLoadException exc("bad driver");
    EXPECT_EQ(exc.get_name(), "PluginLoadException");
}

TEST(ExceptionTest, PluginLoadException_CatchByYaddnscException) {
    try {
        throw PluginLoadException("bad driver");
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
    EXPECT_EQ(exc.get_error(), DnsError::UNKNOWN);
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode) {
    DnsLookupException exc("nxdomain", DnsError::NX_DOMAIN);
    EXPECT_EQ(exc.get_error(), DnsError::NX_DOMAIN);
    EXPECT_EQ(std::string_view(exc.what()), "nxdomain");
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode_Retry) {
    DnsLookupException exc("timeout", DnsError::RETRY);
    EXPECT_EQ(exc.get_error(), DnsError::RETRY);
}

TEST(ExceptionTest, DnsLookupException_WrapYaddnscException) {
    PluginLoadException inner("inner");
    DnsLookupException wrapped(std::move(inner), DnsError::CONNECTION);
    EXPECT_EQ(wrapped.get_error(), DnsError::CONNECTION);
    EXPECT_EQ(std::string_view(wrapped.what()), "inner");
}

TEST(ExceptionTest, DnsLookupException_WrapConstYaddnscException) {
    const PluginLoadException inner("inner");
    DnsLookupException wrapped(inner, DnsError::CONFIG);
    EXPECT_EQ(wrapped.get_error(), DnsError::CONFIG);
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
    static_assert(std::is_base_of_v<YaddnscException, PluginLoadException>);
    static_assert(std::is_base_of_v<YaddnscException, ConfigVerificationException>);
    static_assert(std::is_base_of_v<YaddnscException, DnsLookupException>);
    static_assert(std::is_base_of_v<YaddnscException, SocketException>);
}

TEST(ExceptionTest, AllExceptions_What_IsNonNull) {
    PluginLoadException bd("bd");
    ConfigVerificationException cv("cv");
    DnsLookupException dl("dl");
    SocketException sk("sk");

    EXPECT_NE(bd.what(), nullptr);
    EXPECT_NE(cv.what(), nullptr);
    EXPECT_NE(dl.what(), nullptr);
    EXPECT_NE(sk.what(), nullptr);
}
