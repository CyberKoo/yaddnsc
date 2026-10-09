// Unit tests for uri.h / uri.cpp — edge cases, origin, and accessors.
//
// Verifies:
//   - get_origin with default / non-default port, with and without scheme.
//   - get_raw_uri preservation.
//   - get_body behaviour.
//   - Bare IPv6 (unbracketed) in authority context.
//   - Malformed-input rejection: invalid scheme, invalid characters, empty
//     host, unclosed / invalid IPv6 literals, garbage after ']', non-numeric
//     and out-of-range ports.
// =============================================================================

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/uri/uri.h"

// ===========================================================================
// Origin
// ===========================================================================

TEST(UriEdgeTest, Origin_HttpsDefaultPort) {
    auto uri = Uri::parse("https://example.com/path").value();
    EXPECT_EQ(uri.get_origin(), "https://example.com");
}

TEST(UriEdgeTest, Origin_NonDefaultPort) {
    auto uri = Uri::parse("https://example.com:8443/path").value();
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

TEST(UriEdgeTest, Origin_NoScheme) {
    auto uri = Uri::parse("example.com:8080").value();
    EXPECT_TRUE(uri.get_origin().find("example.com") != std::string_view::npos);
    EXPECT_TRUE(uri.get_origin().find("8080") != std::string_view::npos);
}

TEST(UriEdgeTest, Origin_NoSchemeNoPort) {
    auto uri = Uri::parse("example.com").value();
    EXPECT_EQ(uri.get_port(), 0);
    EXPECT_EQ(uri.get_origin(), "example.com");
}

TEST(UriEdgeTest, Origin_NoSchemeWithNonDefaultPort) {
    auto uri = Uri::parse("example.com:8080").value();
    EXPECT_EQ(uri.get_origin(), "example.com:8080");
}

TEST(UriEdgeTest, Origin_SchemeWithNonMatchingPort) {
    auto uri = Uri::parse("https://example.com:8443").value();
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

TEST(UriEdgeTest, Origin_SchemeDefaultPort) {
    auto uri = Uri::parse("http://example.com").value();
    EXPECT_EQ(uri.get_origin(), "http://example.com");
}

// ===========================================================================
// Accessors
// ===========================================================================

TEST(UriEdgeTest, GetRawUri) {
    const std::string raw = "https://example.com/path?q=1";
    auto uri = Uri::parse(raw).value();
    EXPECT_EQ(uri.get_raw_uri(), raw);
}

TEST(UriEdgeTest, GetBody) {
    auto uri = Uri::parse("https://example.com/path").value();
    EXPECT_FALSE(uri.get_body().empty());
}

// ===========================================================================
// IPv6 edge cases
// ===========================================================================

TEST(UriEdgeTest, NoSchemeIPv6Bracketed) {
    auto uri = Uri::parse("[::1]:853").value();
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 853);
}

TEST(UriEdgeTest, BareIPv6NoBrackets) {
    auto uri = Uri::parse("http://::1").value();
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, BareIPv6BareAddressNoScheme) {
    auto uri = Uri::parse("2001:db8::1").value();
    EXPECT_TRUE(uri.get_schema().empty());
}

TEST(UriEdgeTest, UnclosedIPv6_ReturnsError) {
    const auto uri = Uri::parse("http://[::1");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::UNCLOSED_IPV6_BRACKET);
}

TEST(UriEdgeTest, BracketIPv6_WithPort_AfterClosingBracket_AndPath) {
    auto uri = Uri::parse("https://[::1]:8443/path?query=1").value();
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8443);
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "query=1");
}

TEST(UriEdgeTest, BracketIPv6_WithPort_AfterClosingBracket) {
    auto uri = Uri::parse("http://[::1]:8080/path").value();
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8080);
}

TEST(UriEdgeTest, BracketIPv6_WithTrailingColonNoPort) {
    auto uri = Uri::parse("http://[::1]:").value();
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, BareIPv6_InAuthority_NoPort) {
    auto uri = Uri::parse("http://2001:db8::1").value();
    EXPECT_EQ(uri.get_host(), "2001:db8::1");
}

// ===========================================================================
// Host:port edge cases
// ===========================================================================

TEST(UriEdgeTest, HostPort_WithTrailingColonNoPort) {
    auto uri = Uri::parse("http://example.com:").value();
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, HostPort_WithNonNumericPort_ReturnsError) {
    const auto uri = Uri::parse("http://example.com:abc");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_NOT_NUMERIC);
}

// ===========================================================================
// Port range validation
// ===========================================================================

TEST(UriEdgeTest, HostPort_OutOfRangePort_ReturnsError) {
    // Regression: 99999 was previously accepted and silently truncated to
    // 34463 by static_cast<uint16_t> at the call site.
    const auto uri = Uri::parse("http://example.com:99999");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_OUT_OF_RANGE);
}

TEST(UriEdgeTest, HostPort_NegativePort_ReturnsError) {
    // '-' is a non-digit character in port position.
    const auto uri = Uri::parse("http://example.com:-1");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_NOT_NUMERIC);
}

TEST(UriEdgeTest, BracketIPv6_OutOfRangePort_ReturnsError) {
    const auto uri = Uri::parse("http://[::1]:70000");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_OUT_OF_RANGE);
}

TEST(UriEdgeTest, HostPort_TrailingGarbage_ReturnsError) {
    // "8080x" is malformed: reject it outright rather than silently accepting
    // 8080 or dropping the port in favour of the default.
    const auto uri = Uri::parse("http://example.com:8080x");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_NOT_NUMERIC);
}

TEST(UriEdgeTest, HostPort_MaxPort_Accepted) {
    auto uri = Uri::parse("http://example.com:65535").value();
    EXPECT_EQ(uri.get_port(), 65535);
}

TEST(UriEdgeTest, HostPort_ZeroPort_Accepted) {
    // Port 0 means "unspecified" throughout the code base.
    auto uri = Uri::parse("http://example.com:0").value();
    EXPECT_EQ(uri.get_port(), 0);
}

// ===========================================================================
// Scheme validation
// ===========================================================================

TEST(UriEdgeTest, SchemeStartingWithDigit_ReturnsError) {
    const auto uri = Uri::parse("1abc://example.com");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_SCHEME);
}

TEST(UriEdgeTest, SchemeWithIllegalCharacter_ReturnsError) {
    // '_' is not in the scheme grammar (ALPHA / DIGIT / '+' / '-' / '.').
    const auto uri = Uri::parse("ht_tp://example.com");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_SCHEME);
}

TEST(UriEdgeTest, SchemeWithPlusDotDash_Accepted) {
    auto uri = Uri::parse("a+b-c.d://example.com").value();
    EXPECT_EQ(uri.get_schema(), "a+b-c.d");
}

// ===========================================================================
// Invalid characters
// ===========================================================================

TEST(UriEdgeTest, SpaceInHost_ReturnsError) {
    const auto uri = Uri::parse("http://ex ample.com/");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_CHARACTER);
}

TEST(UriEdgeTest, ControlCharacterInPath_ReturnsError) {
    const auto uri = Uri::parse("http://example.com/\tpath");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_CHARACTER);
}

TEST(UriEdgeTest, Utf8Bytes_Accepted) {
    // Bytes >= 0x80 are tolerated (IRI convention).
    auto uri = Uri::parse("http://example.com/\xe4\xbd\xa0\xe5\xa5\xbd").value();
    EXPECT_EQ(uri.get_path(), "/\xe4\xbd\xa0\xe5\xa5\xbd");
}

// ===========================================================================
// Empty host
// ===========================================================================

TEST(UriEdgeTest, SchemeWithPortButNoHost_ReturnsError) {
    const auto uri = Uri::parse("http://:8080/path");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::EMPTY_HOST);
}

TEST(UriEdgeTest, BarePortNoScheme_ReturnsError) {
    const auto uri = Uri::parse(":8080");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::EMPTY_HOST);
}

// ===========================================================================
// IPv6 literal validation
// ===========================================================================

TEST(UriEdgeTest, BracketedNonIPAddress_ReturnsError) {
    const auto uri = Uri::parse("http://[not-an-ip]/");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_IPV6_LITERAL);
}

TEST(UriEdgeTest, EmptyBrackets_ReturnsError) {
    const auto uri = Uri::parse("http://[]/");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::INVALID_IPV6_LITERAL);
}

TEST(UriEdgeTest, GarbageAfterClosingBracket_ReturnsError) {
    const auto uri = Uri::parse("http://[::1]xyz/");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::IPV6_TRAILING_GARBAGE);
}

TEST(UriEdgeTest, BracketIPv6_NonNumericPort_ReturnsError) {
    const auto uri = Uri::parse("http://[::1]:12x");
    ASSERT_FALSE(uri.has_value());
    EXPECT_EQ(uri.error(), UriError::PORT_NOT_NUMERIC);
}

// ===========================================================================
// Path-only references
// ===========================================================================

TEST(UriEdgeTest, RelativePathWithQuery_SplitCorrectly) {
    auto uri = Uri::parse("/api/v1?key=val&x=1").value();
    EXPECT_EQ(uri.get_path(), "/api/v1");
    EXPECT_EQ(uri.get_query_string(), "key=val&x=1");
}

TEST(UriEdgeTest, DotRelativePathWithQuery_SplitCorrectly) {
    auto uri = Uri::parse("./a?b=1").value();
    EXPECT_EQ(uri.get_path(), "./a");
    EXPECT_EQ(uri.get_query_string(), "b=1");
}

// ===========================================================================
// error_message coverage
// ===========================================================================

TEST(UriEdgeTest, ErrorMessage_CoversAllValues) {
    for (const auto err : {UriError::INVALID_CHARACTER, UriError::INVALID_SCHEME, UriError::EMPTY_HOST,
                           UriError::UNCLOSED_IPV6_BRACKET, UriError::INVALID_IPV6_LITERAL,
                           UriError::IPV6_TRAILING_GARBAGE, UriError::PORT_NOT_NUMERIC, UriError::PORT_OUT_OF_RANGE}) {
        EXPECT_FALSE(error_message(err).empty()) << static_cast<int>(err);
    }
}
