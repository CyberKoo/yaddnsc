//
// Unit tests for util/bytes.hpp — Utils::Bytes big-endian read helpers.
//
// Verifies:
//   - read_u16_be returns correct values for known byte patterns.
//   - read_u32_be returns correct values for known byte patterns.
//   - All three overload variants (raw pointer, span, span+offset) agree.
//   - Leading zeros are handled correctly.
//   - Maximum values fit within the return type.
// =============================================================================

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "util/bytes.hpp"

// ── read_u16_be ───────────────────────────────────────────────────────────────

TEST(BytesTest, ReadU16_SimpleValue) {
    const std::uint8_t buf[] = {0x12, 0x34};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0x1234U);
}

TEST(BytesTest, ReadU16_FromSpan) {
    const std::uint8_t buf[] = {0xAB, 0xCD};
    std::span<const std::uint8_t> s{buf};
    EXPECT_EQ(Utils::Bytes::read_u16_be(s), 0xABCDU);
}

TEST(BytesTest, ReadU16_FromSpanWithOffset) {
    const std::uint8_t buf[] = {0x00, 0x00, 0xDE, 0xAD};
    std::span<const std::uint8_t> s{buf};
    EXPECT_EQ(Utils::Bytes::read_u16_be(s, 2), 0xDEADU);
}

TEST(BytesTest, ReadU16_Zero) {
    const std::uint8_t buf[] = {0x00, 0x00};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0x0000U);
}

TEST(BytesTest, ReadU16_MaxValue) {
    const std::uint8_t buf[] = {0xFF, 0xFF};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0xFFFFU);
}

TEST(BytesTest, ReadU16_SingleByte_LeadingZero) {
    const std::uint8_t buf[] = {0x00, 0x01};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0x0001U);
}

TEST(BytesTest, ReadU16_RawPointerMatchesSpan) {
    const std::uint8_t buf[] = {0xCA, 0xFE};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), Utils::Bytes::read_u16_be(std::span{buf}));
}

// ── read_u32_be ───────────────────────────────────────────────────────────────

TEST(BytesTest, ReadU32_SimpleValue) {
    const std::uint8_t buf[] = {0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(Utils::Bytes::read_u32_be(buf), 0x01020304UL);
}

TEST(BytesTest, ReadU32_FromSpan) {
    const std::uint8_t buf[] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::span<const std::uint8_t> s{buf};
    EXPECT_EQ(Utils::Bytes::read_u32_be(s), 0xDEADBEEFUL);
}

TEST(BytesTest, ReadU32_FromSpanWithOffset) {
    const std::uint8_t buf[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xEE, 0x01};
    std::span<const std::uint8_t> s{buf};
    EXPECT_EQ(Utils::Bytes::read_u32_be(s, 4), 0xC0FFEE01UL);
}

TEST(BytesTest, ReadU32_Zero) {
    const std::uint8_t buf[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(Utils::Bytes::read_u32_be(buf), 0x00000000UL);
}

TEST(BytesTest, ReadU32_MaxValue) {
    const std::uint8_t buf[] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(Utils::Bytes::read_u32_be(buf), 0xFFFFFFFFUL);
}

TEST(BytesTest, ReadU32_RawPointerMatchesSpan) {
    const std::uint8_t buf[] = {0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_EQ(Utils::Bytes::read_u32_be(buf), Utils::Bytes::read_u32_be(std::span{buf}));
}

// ── constexpr verification ────────────────────────────────────────────────────

TEST(BytesTest, ReadU16_Constexpr) {
    constexpr std::uint8_t buf[] = {0x11, 0x22};
    constexpr auto result = Utils::Bytes::read_u16_be(buf);
    EXPECT_EQ(result, 0x1122U);
}

TEST(BytesTest, ReadU32_Constexpr) {
    constexpr std::uint8_t buf[] = {0xA1, 0xB2, 0xC3, 0xD4};
    constexpr auto result = Utils::Bytes::read_u32_be(buf);
    EXPECT_EQ(result, 0xA1B2C3D4UL);
}
