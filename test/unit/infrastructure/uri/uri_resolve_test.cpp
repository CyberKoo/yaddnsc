#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "infrastructure/uri/uri.h"

TEST(UriResolveTest, Resolve_Rfc3986Examples_ResolvesAuthorityBasedReferences) {
    const auto base = Uri::parse("http://a/b/c/d;p?q").value();
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g/"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q"},
        {"g#s", "http://a/b/c/g"},
        {"g?y#s", "http://a/b/c/g?y"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {"g;x?y#s", "http://a/b/c/g;x?y"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g"},
        {"g#s/../x", "http://a/b/c/g"},
    };
    for (const auto& [reference, expected] : cases) {
        SCOPED_TRACE(reference);
        const auto resolved = base.resolve(reference);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->get_raw_uri(), expected);
    }
}

TEST(UriResolveTest, Resolve_PathBoundaries_PreservesSlashesEscapesAndEmptyQueries) {
    const auto base = Uri::parse("https://example.test/a//b?q").value();
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"next", "https://example.test/a//next"},
        {"/x//y/", "https://example.test/x//y/"},
        {"/x/.", "https://example.test/x/"},
        {"/x/..", "https://example.test/"},
        {"/x//../y", "https://example.test/x/y"},
        {"/%2e/%2E%2E/x", "https://example.test/%2e/%2E%2E/x"},
        {"?", "https://example.test/a//b?"},
        {"?#frag", "https://example.test/a//b?"},
        {"/x?", "https://example.test/x?"},
        {"#frag", "https://example.test/a//b?q"},
        {"https://Other.test/x/../y?#f", "https://other.test/y?"},
        {"//Other.test:8080/x/../y", "https://other.test:8080/y"},
    };
    for (const auto& [reference, expected] : cases) {
        SCOPED_TRACE(reference);
        const auto resolved = base.resolve(reference);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->get_raw_uri(), expected);
    }
    const auto empty_query = Uri::parse("https://example.test/a?").value();
    EXPECT_TRUE(empty_query.has_query());
    EXPECT_EQ(empty_query.resolve("#fragment")->get_raw_uri(), "https://example.test/a?");
    EXPECT_FALSE(base.resolve("next")->has_query());
    const auto dotted_base = Uri::parse("https://example.test/a/../b?q").value();
    EXPECT_EQ(dotted_base.resolve("#f")->get_path(), "/a/../b");
}

TEST(UriResolveTest, Resolve_AuthorityAndLifetime_PreservesPortsAndOwnsResult) {
    auto result = Uri::parse("https://[::1]:8443").value().resolve("next");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_host(), "::1");
    EXPECT_EQ(result->get_port(), 8443);
    EXPECT_EQ(result->get_raw_uri(), "https://[::1]:8443/next");
    EXPECT_EQ(result->resolve("//[::2]/next")->get_port(), 443);
    EXPECT_EQ(Uri::parse("http://a:0/x")->resolve("y")->get_port(), 0);
    EXPECT_EQ(Uri::parse("tls://a:853/x")->resolve("y")->get_schema(), "tls");
}

TEST(UriResolveTest, Resolve_InvalidInputs_ReturnsSpecificErrors) {
    const auto base = Uri::parse("https://example.test/a").value();
    const std::pair<std::string_view, UriError> cases[] = {
        {"a b", UriError::INVALID_CHARACTER},
        {"x\r\ny", UriError::INVALID_CHARACTER},
        {"#bad fragment", UriError::INVALID_CHARACTER},
        {"1bad://a/x", UriError::INVALID_SCHEME},
        {":x", UriError::INVALID_SCHEME},
        {"mailto:a", UriError::UNSUPPORTED_REFERENCE},
        {"http:g", UriError::UNSUPPORTED_REFERENCE},
        {"https:///x", UriError::EMPTY_HOST},
        {"//", UriError::EMPTY_HOST},
        {"//a:65536", UriError::PORT_OUT_OF_RANGE},
        {"//a:bad", UriError::PORT_NOT_NUMERIC},
        {"//[invalid]/", UriError::INVALID_IPV6_LITERAL},
    };
    for (const auto& [reference, expected] : cases) {
        SCOPED_TRACE(reference);
        const auto resolved = base.resolve(reference);
        ASSERT_FALSE(resolved.has_value());
        EXPECT_EQ(resolved.error(), expected);
        EXPECT_FALSE(error_message(resolved.error()).empty());
    }
    for (const auto input : {"", "example.test/path", "/path"}) {
        const auto resolved = Uri::parse(input)->resolve("next");
        ASSERT_FALSE(resolved.has_value());
        EXPECT_EQ(resolved.error(), UriError::INVALID_BASE_URI);
    }
}
