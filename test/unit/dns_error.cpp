//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns_error.h / dns/error.cpp — DNS error codes.
//
// Verifies:
//   - All Error enum values are defined.
//   - error_to_str returns a non-empty string for every enumerator.
//   - error_to_str handles the default (unknown) case.
// =============================================================================

#include <gtest/gtest.h>

#include "dns_error.h"

TEST(DnsErrorTest, ErrorToStr_NxDomain) {
    auto s = DNS::error_to_str(DNS::Error::NX_DOMAIN);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("NXDOMAIN") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Retry) {
    auto s = DNS::error_to_str(DNS::Error::RETRY);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("TRY_AGAIN") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_NoData) {
    auto s = DNS::error_to_str(DNS::Error::NODATA);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("NO_DATA") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Parse) {
    auto s = DNS::error_to_str(DNS::Error::PARSE);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("PARSE") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Connection) {
    auto s = DNS::error_to_str(DNS::Error::CONNECTION);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("CONNECTION") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Config) {
    auto s = DNS::error_to_str(DNS::Error::CONFIG);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("CONFIG") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Unknown) {
    // Cast an out-of-range value to test the default branch.
    auto s = DNS::error_to_str(static_cast<DNS::Error>(99));
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("unknown") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_AllEnums_NonNull) {
    // Every valid enum value should produce a non-empty string.
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::NX_DOMAIN).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::RETRY).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::NODATA).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::PARSE).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::CONNECTION).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::CONFIG).empty());
    EXPECT_FALSE(DNS::error_to_str(DNS::Error::UNKNOWN).empty());
}

// ── Enum properties ──────────────────────────────────────────────────────────

TEST(DnsErrorTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<DNS::Error>));
    EXPECT_FALSE((std::is_convertible_v<DNS::Error, int>));
}
