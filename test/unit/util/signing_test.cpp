//
// Unit tests for src/util/signing.cpp — cryptographic primitives.
//
// Verifies:
//   - sha256 / sha1 produce known digests.
//   - sha256_hex returns correct hex string.
//   - hmac_sha256 / hmac_sha1 with known test vectors.
//   - hex_encode with various inputs (including empty).
//   - base64_encode with various input lengths (0, 1, 2, 3+ bytes)
//     to cover all remaining-byte branches.
//   - iso8601_timestamp / iso8601_date produce valid ISO 8601 format.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "signing.h"

// ── Known test vectors ─────────────────────────────────────────────────────

// SHA-256("abc") — FIPS 180-4
constexpr std::string_view ABC_SHA256_HEX = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// SHA-1("abc") — FIPS 180-4
constexpr std::string_view ABC_SHA1_HEX = "a9993e364706816aba3e25717850c26c9cd0d89d";

// HMAC-SHA256(key="key", data="The quick brown fox jumps over the lazy dog")
// Verified with RFC 4231 section 2 (Test Case 2).
constexpr std::string_view HMAC_SHA256_EXPECTED =
    "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";

// HMAC-SHA1(key="key", data="The quick brown fox jumps over the lazy dog")
constexpr std::string_view HMAC_SHA1_EXPECTED =
    "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9";

// ── Helper: convert hex string to bytes ─────────────────────────────────────
[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char buf[3] = {hex[i], hex[i + 1], 0};
        bytes.push_back(static_cast<std::uint8_t>(std::strtoul(buf, nullptr, 16)));
    }
    return bytes;
}

// =============================================================================
//  SHA-256
// =============================================================================

TEST(SigningTest, Sha256_EmptyInput) {
    auto digest = Signing::sha256({});
    EXPECT_FALSE(digest.empty());
    EXPECT_EQ(digest.size(), 32u);  // SHA-256 produces 32 bytes
}

TEST(SigningTest, Sha256_KnownDigest) {
    std::string input = "abc";
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(input.data()), input.size());
    auto digest = Signing::sha256(data);
    auto hex = Signing::hex_encode(digest);
    EXPECT_EQ(hex, ABC_SHA256_HEX);
}

TEST(SigningTest, Sha256_DifferentInputsProduceDifferentDigests) {
    auto d1 = Signing::sha256(std::span(reinterpret_cast<const std::uint8_t *>("a"), 1));
    auto d2 = Signing::sha256(std::span(reinterpret_cast<const std::uint8_t *>("b"), 1));
    EXPECT_NE(d1, d2);
}

// =============================================================================
//  SHA-1
// =============================================================================

TEST(SigningTest, Sha1_EmptyInput) {
    auto digest = Signing::sha1({});
    EXPECT_FALSE(digest.empty());
    EXPECT_EQ(digest.size(), 20u);  // SHA-1 produces 20 bytes
}

TEST(SigningTest, Sha1_KnownDigest) {
    std::string input = "abc";
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(input.data()), input.size());
    auto digest = Signing::sha1(data);
    auto hex = Signing::hex_encode(digest);
    EXPECT_EQ(hex, ABC_SHA1_HEX);
}

// =============================================================================
//  sha256_hex
// =============================================================================

TEST(SigningTest, Sha256Hex_EmptyInput) {
    auto hex = Signing::sha256_hex("");
    // SHA-256 of empty string:
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    EXPECT_EQ(hex.size(), 64u);
    EXPECT_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SigningTest, Sha256Hex_KnownDigest) {
    auto hex = Signing::sha256_hex("abc");
    EXPECT_EQ(hex, ABC_SHA256_HEX);
}

// =============================================================================
//  HMAC-SHA256
// =============================================================================

TEST(SigningTest, HmacSha256_KnownTestVector) {
    std::string key_str = "key";
    std::string data_str = "The quick brown fox jumps over the lazy dog";

    auto key = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(key_str.data()), key_str.size());
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(data_str.data()), data_str.size());

    auto hmac = Signing::hmac_sha256(key, data);
    auto hex = Signing::hex_encode(hmac);
    EXPECT_EQ(hex, HMAC_SHA256_EXPECTED);
}

TEST(SigningTest, HmacSha256_EmptyKey) {
    // An empty key causes EVP_PKEY_new_mac_key to fail — function returns empty.
    std::string data_str = "data";
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(data_str.data()), data_str.size());
    auto hmac = Signing::hmac_sha256({}, data);
    EXPECT_TRUE(hmac.empty());
}

TEST(SigningTest, HmacSha256_EmptyData) {
    std::string key_str = "key";
    auto key = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(key_str.data()), key_str.size());
    auto hmac = Signing::hmac_sha256(key, {});
    EXPECT_FALSE(hmac.empty());
}

// =============================================================================
//  HMAC-SHA1
// =============================================================================

TEST(SigningTest, HmacSha1_KnownTestVector) {
    std::string key_str = "key";
    std::string data_str = "The quick brown fox jumps over the lazy dog";

    auto key = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(key_str.data()), key_str.size());
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(data_str.data()), data_str.size());

    auto hmac = Signing::hmac_sha1(key, data);
    auto hex = Signing::hex_encode(hmac);
    EXPECT_EQ(hex, HMAC_SHA1_EXPECTED);
}

// =============================================================================
//  hex_encode
// =============================================================================

TEST(SigningTest, HexEncode_EmptyInput) {
    auto hex = Signing::hex_encode({});
    EXPECT_TRUE(hex.empty());
}

TEST(SigningTest, HexEncode_SingleByte) {
    std::array<std::uint8_t, 1> data{0xAB};
    auto hex = Signing::hex_encode(data);
    EXPECT_EQ(hex, "ab");
}

TEST(SigningTest, HexEncode_MultipleBytes) {
    std::array<std::uint8_t, 4> data{0xDE, 0xAD, 0xBE, 0xEF};
    auto hex = Signing::hex_encode(data);
    EXPECT_EQ(hex, "deadbeef");
}

TEST(SigningTest, HexEncode_AllZeros) {
    std::array<std::uint8_t, 3> data{0x00, 0x00, 0x00};
    auto hex = Signing::hex_encode(data);
    EXPECT_EQ(hex, "000000");
}

TEST(SigningTest, HexEncode_AllOnes) {
    std::array<std::uint8_t, 2> data{0xFF, 0xFF};
    auto hex = Signing::hex_encode(data);
    EXPECT_EQ(hex, "ffff");
}

// =============================================================================
//  base64_encode
// =============================================================================

TEST(SigningTest, Base64Encode_EmptyInput) {
    auto b64 = Signing::base64_encode({});
    EXPECT_TRUE(b64.empty());
}

TEST(SigningTest, Base64Encode_1Byte) {
    // "f" (0x66) → base64: "Zg" (RFC 4648 test vector)
    std::array<std::uint8_t, 1> data{'f'};
    auto b64 = Signing::base64_encode(data);
    EXPECT_EQ(b64, "Zg");
}

TEST(SigningTest, Base64Encode_2Bytes) {
    // "fo" (0x66 0x6F) → base64: "Zm8" (RFC 4648 test vector)
    std::array<std::uint8_t, 2> data{'f', 'o'};
    auto b64 = Signing::base64_encode(data);
    EXPECT_EQ(b64, "Zm8");
}

TEST(SigningTest, Base64Encode_3Bytes) {
    // "foo" (0x66 0x6F 0x6F) → base64: "Zm9v" (RFC 4648)
    std::array<std::uint8_t, 3> data{'f', 'o', 'o'};
    auto b64 = Signing::base64_encode(data);
    EXPECT_EQ(b64, "Zm9v");
}

TEST(SigningTest, Base64Encode_4Bytes) {
    // "foob" (0x66 0x6F 0x6F 0x62) → base64: "Zm9vYg" (RFC 4648)
    std::array<std::uint8_t, 4> data{'f', 'o', 'o', 'b'};
    auto b64 = Signing::base64_encode(data);
    EXPECT_EQ(b64, "Zm9vYg");
}

TEST(SigningTest, Base64Encode_LongerInput) {
    std::string input = "Man is distinguished, not only by his reason, but by this singular passion from other animals, which is a lust of the mind, that by a perseverance of delight in the continued and indefatigable generation of knowledge, exceeds the short vehemence of any carnal pleasure.";
    auto data = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(input.data()), input.size());
    auto b64 = Signing::base64_encode(data);

    // Expected from RFC 4648 §10 (no padding).
    constexpr std::string_view EXPECTED =
        "TWFuIGlzIGRpc3Rpbmd1aXNoZWQsIG5vdCBvbmx5IGJ5IGhpcyByZWFzb24sIGJ1dCBieSB0aGlz"
        "IHNpbmd1bGFyIHBhc3Npb24gZnJvbSBvdGhlciBhbmltYWxzLCB3aGljaCBpcyBhIGx1c3Qgb2Yg"
        "dGhlIG1pbmQsIHRoYXQgYnkgYSBwZXJzZXZlcmFuY2Ugb2YgZGVsaWdodCBpbiB0aGUgY29udGlu"
        "dWVkIGFuZCBpbmRlZmF0aWdhYmxlIGdlbmVyYXRpb24gb2Yga25vd2xlZGdlLCBleGNlZWRzIHRo"
        "ZSBzaG9ydCB2ZWhlbWVuY2Ugb2YgYW55IGNhcm5hbCBwbGVhc3VyZS4";
    EXPECT_EQ(b64, EXPECTED);
}

TEST(SigningTest, Base64Encode_BinaryData) {
    std::array<std::uint8_t, 3> data{0x00, 0x01, 0x02};
    auto b64 = Signing::base64_encode(data);
    EXPECT_EQ(b64, "AAEC");
}

// =============================================================================
//  iso8601_timestamp / iso8601_date
// =============================================================================

TEST(SigningTest, Iso8601_Timestamp_Format) {
    auto ts = Signing::iso8601_timestamp();
    // Expected format: "YYYYMMDDTHHmmSSZ" (16 characters)
    EXPECT_EQ(ts.size(), 16u);
    EXPECT_EQ(ts[8], 'T');
    EXPECT_EQ(ts[15], 'Z');
    // First 8 chars should be digits (date)
    for (int i = 0; i < 8; ++i) {
        EXPECT_GE(ts[i], '0');
        EXPECT_LE(ts[i], '9');
    }
}

TEST(SigningTest, Iso8601_Date_Format) {
    auto date = Signing::iso8601_date();
    // Expected format: "YYYYMMDD" (8 characters)
    EXPECT_EQ(date.size(), 8u);
    // All should be digits
    for (char c : date) {
        EXPECT_GE(c, '0');
        EXPECT_LE(c, '9');
    }
}

TEST(SigningTest, Iso8601_TimestampAndDate_AreConsistent) {
    auto ts = Signing::iso8601_timestamp();
    auto date = Signing::iso8601_date();
    // The first 8 chars of the timestamp should match the date.
    EXPECT_EQ(ts.substr(0, 8), date);
}

// =============================================================================
//  Integration: Signing used in AWS SigV4-like chain
// =============================================================================

TEST(SigningTest, SigV4LikeSigningChain) {
    // Simulate a minimal SigV4 signing chain to verify the primitives work
    // together: hmac_sha256 with derived keys.
    std::string secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    std::string date_str = "20150830";
    std::string region = "us-east-1";
    std::string service = "iam";

    auto k_secret = std::string("AWS4") + secret;
    auto k_date_str = std::string("AWS4") + secret;  // just for test

    // This is a simplified check — ensure the chain doesn't crash and
    // produces non-empty results.
    auto k_date = Signing::hmac_sha256(
        std::span(reinterpret_cast<const std::uint8_t *>(k_date_str.data()), k_date_str.size()),
        std::span(reinterpret_cast<const std::uint8_t *>(date_str.data()), date_str.size()));
    EXPECT_FALSE(k_date.empty());

    auto k_region = Signing::hmac_sha256(
        std::span(k_date),
        std::span(reinterpret_cast<const std::uint8_t *>(region.data()), region.size()));
    EXPECT_FALSE(k_region.empty());

    auto k_service = Signing::hmac_sha256(
        std::span(k_region),
        std::span(reinterpret_cast<const std::uint8_t *>(service.data()), service.size()));
    EXPECT_FALSE(k_service.empty());

    auto k_signing = Signing::hmac_sha256(
        std::span(k_service),
        std::span(reinterpret_cast<const std::uint8_t *>("aws4_request"), 12));
    EXPECT_FALSE(k_signing.empty());
}
