//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for uri.h / uri.cpp — URI parsing (scheme, host, port, path).
//
// Verifies:
//   - Parsing of well-formed URIs (scheme, host, port, path, query, fragment).
//   - Default ports and omission.
//   - IPv6 literal addresses.
//   - Path extraction with and without scheme.
// =============================================================================

#include <string_view>

#include <gtest/gtest.h>

#include "uri.h"

// ===========================================================================
// Basic parsing
// ===========================================================================

TEST(UriParseTest, SimpleHttp) {
    auto uri = Uri::parse("http://example.com/path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_port(), 80);  // default for http
}

TEST(UriParseTest, HttpsWithPort) {
    auto uri = Uri::parse("https://example.com:8443/api/v1");
    EXPECT_EQ(uri.get_schema(), "https");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 8443);
    EXPECT_EQ(uri.get_path(), "/api/v1");
}

TEST(UriParseTest, DefaultHttpsPort) {
    auto uri = Uri::parse("https://example.com");
    EXPECT_EQ(uri.get_port(), 443);  // default for https
}

TEST(UriParseTest, WithQueryString) {
    auto uri = Uri::parse("https://example.com/path?key=value&foo=bar");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "key=value&foo=bar");
}

TEST(UriParseTest, WithFragmentIgnored) {
    auto uri = Uri::parse("http://example.com/path#section");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_TRUE(uri.get_query_string().empty());
}

// ===========================================================================
// Host and authority
// ===========================================================================

TEST(UriParseTest, HostOnly) {
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/");
}

TEST(UriParseTest, HostnameIsLowercased) {
    auto uri = Uri::parse("HTTP://EXAMPLE.COM/Path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
}

TEST(UriParseTest, IPv6Literal) {
    auto uri = Uri::parse("http://[::1]:8080/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8080);
}

TEST(UriParseTest, IPv6LiteralDefaultPort) {
    auto uri = Uri::parse("https://[::1]/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 443);
}

TEST(UriParseTest, BareIPv6NoScheme) {
    auto uri = Uri::parse("[::1]:53");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 53);
}

TEST(UriParseTest, HostPortNoScheme) {
    auto uri = Uri::parse("example.com:8080");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 8080);
    EXPECT_TRUE(uri.get_schema().empty());
}

TEST(UriParseTest, HostNoPortNoScheme) {
    auto uri = Uri::parse("example.com");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 0);
}

TEST(UriParseTest, TLSPort) {
    auto uri = Uri::parse("tls://1.1.1.1:853");
    EXPECT_EQ(uri.get_schema(), "tls");
    EXPECT_EQ(uri.get_host(), "1.1.1.1");
    EXPECT_EQ(uri.get_port(), 853);
}

TEST(UriParseTest, UnknownScheme) {
    auto uri = Uri::parse("unknown://host");
    EXPECT_EQ(uri.get_schema(), "unknown");
    EXPECT_EQ(uri.get_host(), "host");
    EXPECT_EQ(uri.get_port(), 0);
}

TEST(UriParseTest, SchemeOnlyEmptyAuthority) {
    auto uri = Uri::parse("http://");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriParseTest, EmptyAuthorityWithScheme) {
    auto uri = Uri::parse("http:///path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriParseTest, Empty_ReturnsDefault) {
    auto uri = Uri::parse("");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_TRUE(uri.get_path().empty());
}

// ===========================================================================
// Path
// ===========================================================================

TEST(UriParseTest, PathOnly) {
    auto uri = Uri::parse("/absolute/path");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_path(), "/absolute/path");
}

TEST(UriParseTest, RelativePathNoScheme) {
    auto uri = Uri::parse("./relative/path");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_EQ(uri.get_path(), "./relative/path");
}

TEST(UriParseTest, RootPath) {
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_path(), "/");
}

TEST(UriParseTest, EmptyPathWithQuery) {
    auto uri = Uri::parse("http://example.com?query=1");
    EXPECT_EQ(uri.get_path(), "/");
    EXPECT_EQ(uri.get_query_string(), "query=1");
}

TEST(UriParseTest, QueryBeforePath_DefaultPath) {
    auto uri = Uri::parse("http://example.com?query/path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_query_string(), "query/path");
    EXPECT_EQ(uri.get_path(), "/");
}

TEST(UriParseTest, PathBeforeQuery_NormalBehavior) {
    auto uri = Uri::parse("http://example.com/path?query");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "query");
}

TEST(UriParseTest, QueryWithoutPath_DefaultPath) {
    auto uri = Uri::parse("http://example.com?query");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_query_string(), "query");
    EXPECT_EQ(uri.get_path(), "/");
}
