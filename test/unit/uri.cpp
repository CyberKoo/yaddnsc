//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for uri.h / uri.cpp — RFC 3986 URI parser.
//
// Verifies:
//   - Parsing of well-formed URIs (scheme, host, port, path, query, fragment).
//   - Default ports and omission.
//   - IPv6 literal addresses.
//   - Percent-encoding / decoding.
//   - Query parameter extraction.
//   - Edge cases: empty input, path-only inputs, no-scheme inputs.
// =============================================================================

#include <string_view>

#include <gtest/gtest.h>

#include "uri.h"

// ===========================================================================
// Basic parsing
// ===========================================================================

TEST(UriTest, Parse_SimpleHttp) {
    auto uri = Uri::parse("http://example.com/path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_port(), 80);  // default for http
}

TEST(UriTest, Parse_HttpsWithPort) {
    auto uri = Uri::parse("https://example.com:8443/api/v1");
    EXPECT_EQ(uri.get_schema(), "https");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 8443);
    EXPECT_EQ(uri.get_path(), "/api/v1");
}

TEST(UriTest, Parse_DefaultHttpsPort) {
    auto uri = Uri::parse("https://example.com");
    EXPECT_EQ(uri.get_port(), 443);  // default for https
}

TEST(UriTest, Parse_WithQueryString) {
    auto uri = Uri::parse("https://example.com/path?key=value&foo=bar");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "key=value&foo=bar");
}

TEST(UriTest, Parse_WithFragment_Ignored) {
    auto uri = Uri::parse("http://example.com/path#section");
    EXPECT_EQ(uri.get_path(), "/path");
    // Fragment is removed; query string is empty.
    EXPECT_TRUE(uri.get_query_string().empty());
}

// ===========================================================================
// Host and authority
// ===========================================================================

TEST(UriTest, Parse_HostOnly) {
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/");  // default path with scheme
}

TEST(UriTest, Parse_HostnameIsLowercased) {
    auto uri = Uri::parse("HTTP://EXAMPLE.COM/Path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
}

TEST(UriTest, Parse_IPv6Literal) {
    auto uri = Uri::parse("http://[::1]:8080/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8080);
}

TEST(UriTest, Parse_IPv6LiteralDefaultPort) {
    auto uri = Uri::parse("https://[::1]/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 443);
}

TEST(UriTest, Parse_BareIPv6_NoScheme) {
    // When there's no scheme, "::1" is treated as bare IPv6 authority.
    auto uri = Uri::parse("[::1]:53");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 53);
    // No scheme → no default port override.
}

TEST(UriTest, Parse_HostPortNoScheme) {
    auto uri = Uri::parse("example.com:8080");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 8080);
    EXPECT_TRUE(uri.get_schema().empty());
}

TEST(UriTest, Parse_HostNoPortNoScheme) {
    auto uri = Uri::parse("example.com");
    EXPECT_EQ(uri.get_host(), "example.com");
    // Without a scheme, port defaults to 0 ("no default known").
    EXPECT_EQ(uri.get_port(), 0);
}

// ===========================================================================
// Path
// ===========================================================================

TEST(UriTest, Parse_PathOnly) {
    auto uri = Uri::parse("/absolute/path");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_path(), "/absolute/path");
}

TEST(UriTest, Parse_RelativePath_NoScheme) {
    auto uri = Uri::parse("./relative/path");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_EQ(uri.get_path(), "./relative/path");
}

TEST(UriTest, Parse_RootPath) {
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_path(), "/");
}

TEST(UriTest, Parse_EmptyPathWithQuery) {
    auto uri = Uri::parse("http://example.com?query=1");
    EXPECT_EQ(uri.get_path(), "/");  // default path
    EXPECT_EQ(uri.get_query_string(), "query=1");
}

// ===========================================================================
// Query parameters
// ===========================================================================

TEST(UriTest, QueryParams_Empty) {
    auto uri = Uri::parse("http://example.com");
    auto params = uri.get_query_params();
    EXPECT_TRUE(params.empty());
}

TEST(UriTest, QueryParams_Single) {
    auto uri = Uri::parse("http://example.com?key=val");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "key");
    EXPECT_EQ(params[0].second, "val");
}

TEST(UriTest, QueryParams_Multiple) {
    auto uri = Uri::parse("http://example.com?a=1&b=2&c=3");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 3U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[0].second, "1");
    EXPECT_EQ(params[1].first, "b");
    EXPECT_EQ(params[2].first, "c");
}

TEST(UriTest, QueryParams_ValueAbsent) {
    auto uri = Uri::parse("http://example.com?key");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "key");
    EXPECT_TRUE(params[0].second.empty());
}

TEST(UriTest, QueryParams_EmptySegments_Skipped) {
    auto uri = Uri::parse("http://example.com?a=1&&b=2");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
}

TEST(UriTest, QueryParams_PlusToSpace) {
    auto uri = Uri::parse("http://example.com?name=hello+world");
    auto params = uri.get_query_params(true);
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].second, "hello world");
}

TEST(UriTest, QueryParams_PlusKeptWhenDisabled) {
    auto uri = Uri::parse("http://example.com?name=hello+world");
    auto params = uri.get_query_params(false);
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].second, "hello+world");
}

TEST(UriTest, QueryParams_PercentDecoded) {
    auto uri = Uri::parse("http://example.com?q=%E4%BD%A0%E5%A5%BD");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    // "%E4%BD%A0%E5%A5%BD" is UTF-8 for 你好
    EXPECT_EQ(params[0].second, "\xe4\xbd\xa0\xe5\xa5\xbd");
}

// ===========================================================================
// Percent encoding / decoding
// ===========================================================================

TEST(UriTest, UrlEncode_Unreserved_Passthrough) {
    EXPECT_EQ(Uri::url_encode("abcABC123-._~"), "abcABC123-._~");
}

TEST(UriTest, UrlEncode_Space) {
    EXPECT_EQ(Uri::url_encode(" "), "%20");
}

TEST(UriTest, UrlEncode_SpecialChars) {
    EXPECT_EQ(Uri::url_encode("hello world"), "hello%20world");
    EXPECT_EQ(Uri::url_encode("a/b"), "a%2Fb");
}

TEST(UriTest, UrlDecode_Simple) {
    EXPECT_EQ(Uri::url_decode("hello%20world"), "hello world");
}

TEST(UriTest, UrlDecode_UnencodedPassthrough) {
    EXPECT_EQ(Uri::url_decode("hello world"), "hello world");
}

TEST(UriTest, UrlDecode_MalformedPercent_Preserved) {
    // "%GG" is not valid hex → preserved as-is.
    EXPECT_EQ(Uri::url_decode("%GG"), "%GG");
}

TEST(UriTest, UrlDecode_TrailingPercent_Preserved) {
    EXPECT_EQ(Uri::url_decode("end%"), "end%");
}

TEST(UriTest, UrlDecode_Empty) {
    EXPECT_TRUE(Uri::url_decode("").empty());
}

TEST(UriTest, UrlEncode_Decode_RoundTrip) {
    const std::string original = "hello world!@#$%^&*()";
    auto encoded = Uri::url_encode(original);
    auto decoded = Uri::url_decode(encoded);
    EXPECT_EQ(decoded, original);
}

// ===========================================================================
// Origin
// ===========================================================================

TEST(UriTest, GetOrigin_HttpsDefaultPort) {
    auto uri = Uri::parse("https://example.com/path");
    EXPECT_EQ(uri.get_origin(), "https://example.com");
}

TEST(UriTest, GetOrigin_NonDefaultPort) {
    auto uri = Uri::parse("https://example.com:8443/path");
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

TEST(UriTest, GetOrigin_NoScheme) {
    auto uri = Uri::parse("example.com:8080");
    // No scheme → origin is just host:port
    EXPECT_TRUE(uri.get_origin().find("example.com") != std::string_view::npos);
    EXPECT_TRUE(uri.get_origin().find("8080") != std::string_view::npos);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST(UriTest, Parse_Empty_ReturnsDefault) {
    auto uri = Uri::parse("");
    EXPECT_TRUE(uri.get_schema().empty());
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_TRUE(uri.get_path().empty());
}

TEST(UriTest, Parse_TLSPort) {
    auto uri = Uri::parse("tls://1.1.1.1:853");
    EXPECT_EQ(uri.get_schema(), "tls");
    EXPECT_EQ(uri.get_host(), "1.1.1.1");
    EXPECT_EQ(uri.get_port(), 853);
}

TEST(UriTest, GetRawUri) {
    const std::string raw = "https://example.com/path?q=1";
    auto uri = Uri::parse(raw);
    EXPECT_EQ(uri.get_raw_uri(), raw);
}

TEST(UriTest, GetBody) {
    auto uri = Uri::parse("https://example.com/path");
    // Body is the whole URI minus the scheme prefix (implementation detail).
    // Just verify it's non-empty when there's a scheme.
    EXPECT_FALSE(uri.get_body().empty());
}

TEST(UriTest, Parse_NoSchemeIPv6_Bracketed) {
    // Bare [::1] with port
    auto uri = Uri::parse("[::1]:853");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 853);
}

TEST(UriTest, Parse_EmptyAuthority_WithScheme) {
    // "http:///path" has an empty authority (auth is empty string)
    auto uri = Uri::parse("http:///path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_port(), 80);  // default for http
}

TEST(UriTest, Parse_BareIPv6_NoBrackets) {
    // Bare IPv6 without brackets — parse_authority handles this as a fallback
    auto uri = Uri::parse("http://::1");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriTest, Parse_BareIPv6_BareAddress_NoScheme) {
    // Bare IPv6 without brackets and without scheme — just verify no crash
    auto uri = Uri::parse("2001:db8::1");
    EXPECT_TRUE(uri.get_schema().empty());
}

TEST(UriTest, Parse_UnclosedIPv6_Throws) {
    // Missing closing bracket on IPv6 literal
    EXPECT_THROW(Uri::parse("http://[::1"), std::runtime_error);
}

TEST(UriTest, Parse_UnknownScheme) {
    // A scheme not in known_ports table
    auto uri = Uri::parse("unknown://host");
    EXPECT_EQ(uri.get_schema(), "unknown");
    EXPECT_EQ(uri.get_host(), "host");
    // Unknown scheme → default_port_for returns 0
    EXPECT_EQ(uri.get_port(), 0);
}

TEST(UriTest, Parse_SchemeOnly_EmptyAuthority) {
    // RFC 3986: A URI with an empty authority after scheme, e.g. "http://"
    auto uri = Uri::parse("http://");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_TRUE(uri.get_host().empty());
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriTest, GetOrigin_SchemeDefaultPort) {
    // get_origin with scheme and default port → scheme://host (omits port)
    auto uri = Uri::parse("http://example.com");
    EXPECT_EQ(uri.get_origin(), "http://example.com");
}

TEST(UriTest, BracketIPv6_WithPort_AfterClosingBracket_AndPath) {
    // Full coverage of bracketed IPv6 port parsing with path and query
    auto uri = Uri::parse("https://[::1]:8443/path?query=1");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8443);
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "query=1");
}

// ===========================================================================
// is_default_port (tested indirectly via get_origin)
// ===========================================================================

TEST(UriTest, BracketIPv6_WithPort_AfterClosingBracket) {
    // Tests the branch: closing+1 < auth.size() && auth[closing+1] == ':'
    auto uri = Uri::parse("http://[::1]:8080/path");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    EXPECT_EQ(uri.get_port(), 8080);
}

TEST(UriTest, BracketIPv6_WithTrailingColon_NoPort) {
    // closing+1 < auth.size() true, auth[closing+1] == ':' true, but port_str is empty
    auto uri = Uri::parse("http://[::1]:");
    EXPECT_EQ(uri.get_host(), "::1");
    EXPECT_EQ(uri.get_host_literal(), "[::1]");
    // Port should default to 80 (http default) since port is empty → no explicit port
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriTest, HostPort_WithTrailingColon_NoPort) {
    // host:port with empty port — colon exists but port string is empty
    auto uri = Uri::parse("http://example.com:");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 80);
}

TEST(UriTest, HostPort_WithNonNumericPort) {
    // host:port with non-numeric port — from_chars fails, port not set
    auto uri = Uri::parse("http://example.com:abc");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_port(), 80);  // defaults to http
}

TEST(UriTest, BareIPv6_InAuthority_NoPort) {
    // Bare IPv6 (already tested but need to ensure it's covered)
    auto uri = Uri::parse("http://2001:db8::1");
    EXPECT_EQ(uri.get_host(), "2001:db8::1");
}

TEST(UriTest, Parse_QueryBeforePath_UsesDefaultPath) {
    // Per RFC 3986, '?' always starts the query component regardless of where
    // '/' appears. When '?' comes before '/', the '/' is part of the query,
    // not a path separator. The path defaults to "/".
    auto uri = Uri::parse("http://example.com?query/path");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_query_string(), "query/path");
    // The '/' in "query/path" is part of the query value, not the path.
    // With no explicit path, default is "/".
    EXPECT_EQ(uri.get_path(), "/");
}

TEST(UriTest, Parse_PathBeforeQuery_NormalBehavior) {
    // Normal case: '/' before '?' → path is extracted, query starts after '?'
    auto uri = Uri::parse("http://example.com/path?query");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_path(), "/path");
    EXPECT_EQ(uri.get_query_string(), "query");
}

TEST(UriTest, Parse_QueryWithoutPath_DefaultsToSlash) {
    // A query with no explicit path → default path "/"
    auto uri = Uri::parse("http://example.com?query");
    EXPECT_EQ(uri.get_schema(), "http");
    EXPECT_EQ(uri.get_host(), "example.com");
    EXPECT_EQ(uri.get_query_string(), "query");
    EXPECT_EQ(uri.get_path(), "/");
}

// ===========================================================================
// get_origin
// ===========================================================================

TEST(UriTest, GetOrigin_NoSchemeNoPort) {
    // No scheme and port 0 → origin is just the host
    auto uri = Uri::parse("example.com");
    EXPECT_EQ(uri.get_port(), 0);
    // get_origin with no scheme and port 0 returns just host_bracketed_
    EXPECT_EQ(uri.get_origin(), "example.com");
}

TEST(UriTest, GetOrigin_NoSchemeWithNonDefaultPort) {
    // No scheme with non-zero port → origin is host:port
    auto uri = Uri::parse("example.com:8080");
    EXPECT_EQ(uri.get_origin(), "example.com:8080");
}

TEST(UriTest, GetOrigin_SchemeWithNonMatchingPort) {
    // Scheme with a non-default port → scheme://host:port
    auto uri = Uri::parse("https://example.com:8443");
    EXPECT_EQ(uri.get_origin(), "https://example.com:8443");
}

// ===========================================================================
// Query parameters edge cases
// ===========================================================================

TEST(UriTest, QueryParams_TrailingAmpersand) {
    auto uri = Uri::parse("http://example.com?a=1&b=2&");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
}

TEST(UriTest, QueryParams_LeadingAmpersand) {
    auto uri = Uri::parse("http://example.com?&a=1");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "a");
}

TEST(UriTest, QueryParams_DoubleAmpersand) {
    auto uri = Uri::parse("http://example.com?a=1&&b=2&c=3&&");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 3U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
    EXPECT_EQ(params[2].first, "c");
}
