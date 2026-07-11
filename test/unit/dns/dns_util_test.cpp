//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns/util.hpp — DNS::Util compile-time helpers.
//
// Verifies:
//   - read_u16_be byte-order correctness.
//   - type_to_record_type mapping.
// =============================================================================

#include <cstdint>
#include <array>

#include <gtest/gtest.h>

#include "dns/util.hpp"
#include "util/bytes.hpp"
#include "record_kind.h"

// ── read_u16_be ──────────────────────────────────────────────────────────────

TEST(DnsUtilTest, ReadU16Be_Simple) {
    //        byte[0]=0x12  byte[1]=0x34  →  0x1234
    const std::uint8_t buf[2] = {0x12, 0x34};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0x1234U);
}

TEST(DnsUtilTest, ReadU16Be_Zero) {
    const std::uint8_t buf[2] = {0x00, 0x00};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0U);
}

TEST(DnsUtilTest, ReadU16Be_Max) {
    const std::uint8_t buf[2] = {0xFF, 0xFF};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0xFFFFU);
}

TEST(DnsUtilTest, ReadU16Be_BigEndian_MSB) {
    // If bytes are {0x80, 0x00} → most-significant bit is set
    const std::uint8_t buf[2] = {0x80, 0x00};
    EXPECT_EQ(Utils::Bytes::read_u16_be(buf), 0x8000U);
}

TEST(DnsUtilTest, ReadU16Be_SpanOffset) {
    const std::array<uint8_t, 6> buf = {0x00, 0x00, 0x00, 0xAB, 0xCD, 0x00};
    std::span<const uint8_t> sp(buf);
    EXPECT_EQ(Utils::Bytes::read_u16_be(sp, 3), 0xABCDU);
}

// ── type_to_record_type ──────────────────────────────────────────────────────

TEST(DnsUtilTest, TypeToRecordType_A_ReturnsA) {
    EXPECT_EQ(DNS::Util::type_to_record_type(RecordKind::A), DNS::RecordType::A);
}

TEST(DnsUtilTest, TypeToRecordType_AAAA_ReturnsAAAA) {
    EXPECT_EQ(DNS::Util::type_to_record_type(RecordKind::AAAA), DNS::RecordType::AAAA);
}

TEST(DnsUtilTest, TypeToRecordType_TXT_ReturnsTXT) {
    EXPECT_EQ(DNS::Util::type_to_record_type(RecordKind::TXT), DNS::RecordType::TXT);
}

TEST(DnsUtilTest, TypeToRecordType_Constexpr) {
    constexpr auto a = DNS::Util::type_to_record_type(RecordKind::A);
    static_assert(a == DNS::RecordType::A);
}
