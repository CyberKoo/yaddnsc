// Unit tests for dns_error.h / dns/error.cpp — DNS error codes.
//
// Verifies:
//   - All Error enum values are defined.
//   - error_to_str returns a non-empty string for every enumerator.
//   - error_to_str handles the default (unknown) case.
// =============================================================================

#include "domain/error/dns_error.h"

#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

TEST(DnsErrorTest, ErrorToStr_NxDomain) {
    auto s = domain::error_to_str(domain::DnsError::NX_DOMAIN);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("NXDOMAIN") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Retry) {
    auto s = domain::error_to_str(domain::DnsError::RETRY);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("TRY_AGAIN") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_NoData) {
    auto s = domain::error_to_str(domain::DnsError::NODATA);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("NO_DATA") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Parse) {
    auto s = domain::error_to_str(domain::DnsError::PARSE);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("PARSE") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Connection) {
    auto s = domain::error_to_str(domain::DnsError::CONNECTION);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("CONNECTION") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Config) {
    auto s = domain::error_to_str(domain::DnsError::CONFIG);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("CONFIG") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_ServerRefused) {
    auto s = domain::error_to_str(domain::DnsError::SERVER_REFUSED);
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("REFUSED") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_Unknown) {
    // Cast an out-of-range value to test the default branch.
    auto s = domain::error_to_str(static_cast<domain::DnsError>(99));
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("unknown") != std::string_view::npos);
}

TEST(DnsErrorTest, ErrorToStr_AllEnums_NonNull) {
    // Every valid enum value should produce a non-empty string.
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::NX_DOMAIN).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::RETRY).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::NODATA).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::PARSE).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::CONNECTION).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::CONFIG).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::SERVER_REFUSED).empty());
    EXPECT_FALSE(domain::error_to_str(domain::DnsError::UNKNOWN).empty());
}

// ── Enum properties ──────────────────────────────────────────────────────────

TEST(DnsErrorTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<domain::DnsError>) );
    EXPECT_FALSE((std::is_convertible_v<domain::DnsError, int>) );
}
