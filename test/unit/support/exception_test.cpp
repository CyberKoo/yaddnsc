// Unit tests for exception classes.
//
// Verifies:
//   - Each exception type can be thrown and caught.
//   - Base type YaddnscException is caught by std::runtime_error.
//   - get_name() returns the correct type name.
//   - Exception-specific accessors work correctly.
// =============================================================================

#include "support/exception.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "domain/error/dns_error.h"
#include "infrastructure/config/config_exception.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/plugin/plugin_load_exception.h"

// ── Base ─────────────────────────────────────────────────────────────────────

TEST(ExceptionTest, YaddnscException_IsRuntimeError) {
    // YaddnscException is abstract (pure virtual get_name()), so we use a
    // concrete subclass to verify the inheritance chain.
    try {
        throw PluginLoadException("base error");
    } catch (const std::runtime_error&) {
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
    } catch (const YaddnscException&) {
        SUCCEED();
    }
}

// ── ConfigException ──────────────────────────────────────────────────────────

TEST(ExceptionTest, ConfigException_GetName_ReturnsCorrectType) {
    ConfigException exc("config invalid");
    EXPECT_EQ(exc.get_name(), "ConfigException");
}

TEST(ExceptionTest, ConfigException_IsYaddnscException) {
    try {
        throw ConfigException("config invalid");
    } catch (const YaddnscException&) {
        SUCCEED();
    }
}

// ── DnsLookupException ───────────────────────────────────────────────────────

TEST(ExceptionTest, DnsLookupException_DefaultConstructor) {
    DnsLookupException exc("dns error");
    EXPECT_EQ(exc.get_name(), "DnsLookupException");
    EXPECT_EQ(exc.get_error(), domain::DnsError::UNKNOWN);
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode) {
    DnsLookupException exc("nxdomain", domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(exc.get_error(), domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(std::string_view(exc.what()), "nxdomain");
}

TEST(ExceptionTest, DnsLookupException_WithErrorCode_Retry) {
    DnsLookupException exc("timeout", domain::DnsError::RETRY);
    EXPECT_EQ(exc.get_error(), domain::DnsError::RETRY);
}

TEST(ExceptionTest, DnsLookupException_WrapYaddnscException) {
    PluginLoadException inner("inner");
    DnsLookupException wrapped(std::move(inner), domain::DnsError::CONNECTION);
    EXPECT_EQ(wrapped.get_error(), domain::DnsError::CONNECTION);
    EXPECT_EQ(std::string_view(wrapped.what()), "inner");
}

TEST(ExceptionTest, DnsLookupException_WrapConstYaddnscException) {
    const PluginLoadException inner("inner");
    DnsLookupException wrapped(inner, domain::DnsError::CONFIG);
    EXPECT_EQ(wrapped.get_error(), domain::DnsError::CONFIG);
}

// ── Inheritance hierarchy ────────────────────────────────────────────────────

TEST(ExceptionTest, InheritanceHierarchy) {
    // Compile-time check: all concrete exception types inherit from YaddnscException.
    static_assert(std::is_base_of_v<YaddnscException, PluginLoadException>);
    static_assert(std::is_base_of_v<YaddnscException, ConfigException>);
    static_assert(std::is_base_of_v<YaddnscException, DnsLookupException>);
}

TEST(ExceptionTest, AllExceptions_What_IsNonNull) {
    PluginLoadException bd("bd");
    ConfigException cv("cv");
    DnsLookupException dl("dl");

    EXPECT_NE(bd.what(), nullptr);
    EXPECT_NE(cv.what(), nullptr);
    EXPECT_NE(dl.what(), nullptr);
}
