// Unit tests for yaddnsc::util percent codec (url_encode.hpp / url_decode.hpp).
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
#include <yaddnsc/util/url_decode.hpp>
#include <yaddnsc/util/url_encode.hpp>

using yaddnsc::util::url_decode;
using yaddnsc::util::url_encode;

// ===========================================================================
// Percent encoding
// ===========================================================================

TEST(UriEncodeTest, UnreservedPassthrough) {
    EXPECT_EQ(url_encode("abcABC123-._~"), "abcABC123-._~");
}

TEST(UriEncodeTest, Space) {
    EXPECT_EQ(url_encode(" "), "%20");
}

TEST(UriEncodeTest, SpecialChars) {
    EXPECT_EQ(url_encode("hello world"), "hello%20world");
    EXPECT_EQ(url_encode("a/b"), "a%2Fb");
}

// ===========================================================================
// Percent decoding
// ===========================================================================

TEST(UriDecodeTest, Simple) {
    EXPECT_EQ(url_decode("hello%20world"), "hello world");
}

TEST(UriDecodeTest, UnencodedPassthrough) {
    EXPECT_EQ(url_decode("hello world"), "hello world");
}

TEST(UriDecodeTest, MalformedPercentPreserved) {
    EXPECT_EQ(url_decode("%GG"), "%GG");
}

TEST(UriDecodeTest, PartialHexPreserved) {
    // '4' parses as hex but 'G' does not — partial parse is still malformed.
    EXPECT_EQ(url_decode("%4G"), "%4G");
}

TEST(UriDecodeTest, MalformedSequenceDoesNotConsumeNextPercent) {
    // The '%' of the following valid sequence must not be eaten.
    EXPECT_EQ(url_decode("%4%41"), "%4A");
}

TEST(UriDecodeTest, TrailingPercentPreserved) {
    EXPECT_EQ(url_decode("end%"), "end%");
}

TEST(UriDecodeTest, Empty) {
    EXPECT_TRUE(url_decode("").empty());
}

// ===========================================================================
// Round-trip
// ===========================================================================

TEST(UriCodecTest, EncodeDecodeRoundTrip) {
    const std::string original = "hello world!@#$%^&*()";
    auto encoded = url_encode(original);
    auto decoded = url_decode(encoded);
    EXPECT_EQ(decoded, original);
}
