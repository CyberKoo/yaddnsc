//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for uri.h / uri.cpp — edge cases, origin, and accessors.
//
// Verifies:
//   - get_origin with default / non-default port, with and without scheme.
//   - get_raw_uri preservation.
//   - get_body behaviour.
//   - Bare IPv6 (unbracketed) in authority context.
//   - Unclosed IPv6 literal rejection.
//   - Bracket IPv6 with trailing colon, non-numeric port.
//   - Host:port with trailing colon, non-numeric port.
// =============================================================================

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "uri.h"

// ===========================================================================
// Origin
// ===========================================================================

TEST(UriEdgeTest, Origin_HttpsDefaultPort) {
    auto uri = Uri::parse("https://example.com/path");
    EXPECT_EQ(uri.get_origin(), "https://example.com");
}

TEST(UriEdgeTest, Origin_NonDefaultPort) {
    auto uri = Uri::parse("https://example.com:8443/path");
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

TEST(UriEdgeTest, Origin_NoScheme) {
    auto uri = Uri::parse("example.com:8080");
    EXPECT_TRUE(uri.get_origin().find("example.com") != std::string_view::npos);
    EXPECT_TRUE(uri.get_origin().find("8080") != std::string_view::npos);
}

TEST(UriEdgeTest, Origin_NoSchemeNoPort) {
    auto uri = Uri::parse("example.com");
    EXPECT_EQ(uri.get_port(), 0);
    EXPECT_EQ(uri.get_origin(), "example.com");
}

TEST(UriEdgeTest, Origin_NoSchemeWithNonDefaultPort) {
    auto uri = Uri::parse("example.com:8080");
    EXPECT_EQ(uri.get_origin(), "example.com:8080");
}

TEST(UriEdgeTest, Origin_SchemeWithNonMatchingPort) {
    auto uri = Uri::parse("https://example.com:8443");
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

TEST(UriEdgeTest, Origin_SchemeDefaultPort) {
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_origin(), "http://example.com");
}

// ===========================================================================
// Accessors
// ===========================================================================

TEST(UriEdgeTest, GetRawUri) {
    const std::string raw = "https://example.com/path?q=1";
    auto uri = Uri::parse(raw);
    EXPECT_EQ(uri.get_raw_uri(), raw);
}

TEST(UriEdgeTest, GetBody) {
    auto uri = Uri::parse("https://example.com/path");
    EXPECT_FALSE(uri.get_body().empty());
}

// ===========================================================================
// IPv6 edge cases
// ===========================================================================

TEST(UriEdgeTest, NoSchemeIPv6Bracketed) {
    auto uri = Uri::parse("[::1]:853");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 853);
}

TEST(UriEdgeTest, BareIPv6NoBrackets) {
    auto uri = Uri::parse("http://::1");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, BareIPv6BareAddressNoScheme) {
    auto uri = Uri::parse("2001:db8::1");
    EXPECT_TRUE(uri.get_schema().empty());
}

TEST(UriEdgeTest, UnclosedIPv6Throws) {
    EXPECT_THROW(Uri::parse("http://[::1"), std::runtime_error);
}

TEST(UriEdgeTest, BracketIPv6_WithPort_AfterClosingBracket_AndPath) {
    auto uri = Uri::parse("https://[::1]:8443/path?query=1");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8443);
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "query=1");
}

TEST(UriEdgeTest, BracketIPv6_WithPort_AfterClosingBracket) {
    auto uri = Uri::parse("http://[::1]:8080/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8080);
}

TEST(UriEdgeTest, BracketIPv6_WithTrailingColonNoPort) {
    auto uri = Uri::parse("http://[::1]:");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, BareIPv6_InAuthority_NoPort) {
    auto uri = Uri::parse("http://2001:db8::1");
    EXPECT_EQ(uri.get_host(), "2001:db8::1");
}

// ===========================================================================
// Host:port edge cases
// ===========================================================================

TEST(UriEdgeTest, HostPort_WithTrailingColonNoPort) {
    auto uri = Uri::parse("http://example.com:");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriEdgeTest, HostPort_WithNonNumericPort) {
    auto uri = Uri::parse("http://example.com:abc");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 80);
}
