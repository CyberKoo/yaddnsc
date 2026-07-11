//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for uri.h / uri.cpp — percent-encoding and decoding.
//
// Verifies:
//   - Unreserved characters pass through without encoding.
//   - Reserved characters are percent-encoded.
//   - Decoding of valid percent-sequences.
//   - Malformed sequences (%GG, trailing %) preserved as-is.
//   - Empty string round-trip.
//   - Full round-trip (encode → decode → original).
// =============================================================================

#include <string>

#include <gtest/gtest.h>

#include "uri.h"

// ===========================================================================
// Percent encoding
// ===========================================================================

TEST(UriEncodeTest, UnreservedPassthrough) {
    EXPECT_EQ(Uri::url_encode("abcABC123-._~"), "abcABC123-._~");
}

TEST(UriEncodeTest, Space) {
    EXPECT_EQ(Uri::url_encode(" "), "%20");
}

TEST(UriEncodeTest, SpecialChars) {
    EXPECT_EQ(Uri::url_encode("hello world"), "hello%20world");
    EXPECT_EQ(Uri::url_encode("a/b"), "a%2Fb");
}

// ===========================================================================
// Percent decoding
// ===========================================================================

TEST(UriDecodeTest, Simple) {
    EXPECT_EQ(Uri::url_decode("hello%20world"), "hello world");
}

TEST(UriDecodeTest, UnencodedPassthrough) {
    EXPECT_EQ(Uri::url_decode("hello world"), "hello world");
}

TEST(UriDecodeTest, MalformedPercentPreserved) {
    EXPECT_EQ(Uri::url_decode("%GG"), "%GG");
}

TEST(UriDecodeTest, TrailingPercentPreserved) {
    EXPECT_EQ(Uri::url_decode("end%"), "end%");
}

TEST(UriDecodeTest, Empty) {
    EXPECT_TRUE(Uri::url_decode("").empty());
}

// ===========================================================================
// Round-trip
// ===========================================================================

TEST(UriCodecTest, EncodeDecodeRoundTrip) {
    const std::string original = "hello world!@#$%^&*()";
    auto encoded = Uri::url_encode(original);
    auto decoded = Uri::url_decode(encoded);
    EXPECT_EQ(decoded, original);
}
