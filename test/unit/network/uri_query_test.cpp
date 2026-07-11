//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for uri.h / uri.cpp — query parameter extraction.
//
// Verifies:
//   - Empty, single, and multiple query parameters.
//   - Value-absent keys.
//   - Empty segments (&&) skipped.
//   - Plus-to-space decoding.
//   - Percent-decoding in query values.
//   - Trailing/leading/double ampersand handling.
// =============================================================================

#include <string_view>

#include <gtest/gtest.h>

#include "uri.h"

// ===========================================================================
// Query parameters — basic
// ===========================================================================

TEST(UriQueryTest, Empty) {
    auto uri = Uri::parse("http://example.com");
    auto params = uri.get_query_params();
    EXPECT_TRUE(params.empty());
}

TEST(UriQueryTest, Single) {
    auto uri = Uri::parse("http://example.com?key=val");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "key");
    EXPECT_EQ(params[0].second, "val");
}

TEST(UriQueryTest, Multiple) {
    auto uri = Uri::parse("http://example.com?a=1&b=2&c=3");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 3U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[0].second, "1");
    EXPECT_EQ(params[1].first, "b");
    EXPECT_EQ(params[2].first, "c");
}

TEST(UriQueryTest, ValueAbsent) {
    auto uri = Uri::parse("http://example.com?key");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "key");
    EXPECT_TRUE(params[0].second.empty());
}

TEST(UriQueryTest, EmptySegmentsSkipped) {
    auto uri = Uri::parse("http://example.com?a=1&&b=2");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
}

TEST(UriQueryTest, PlusToSpace) {
    auto uri = Uri::parse("http://example.com?name=hello+world");
    auto params = uri.get_query_params(true);
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].second, "hello world");
}

TEST(UriQueryTest, PlusKeptWhenDisabled) {
    auto uri = Uri::parse("http://example.com?name=hello+world");
    auto params = uri.get_query_params(false);
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].second, "hello+world");
}

TEST(UriQueryTest, PercentDecoded) {
    auto uri = Uri::parse("http://example.com?q=%E4%BD%A0%E5%A5%BD");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].second, "\xe4\xbd\xa0\xe5\xa5\xbd");
}

// ===========================================================================
// Query parameters — edge cases
// ===========================================================================

TEST(UriQueryTest, TrailingAmpersand) {
    auto uri = Uri::parse("http://example.com?a=1&b=2&");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
}

TEST(UriQueryTest, LeadingAmpersand) {
    auto uri = Uri::parse("http://example.com?&a=1");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0].first, "a");
}

TEST(UriQueryTest, DoubleAmpersand) {
    auto uri = Uri::parse("http://example.com?a=1&&b=2&c=3&&");
    auto params = uri.get_query_params();
    ASSERT_EQ(params.size(), 3U);
    EXPECT_EQ(params[0].first, "a");
    EXPECT_EQ(params[1].first, "b");
    EXPECT_EQ(params[2].first, "c");
}
