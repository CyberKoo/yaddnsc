//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns/util.hpp — DNS::Util compile-time helpers.
//
// Verifies:
//   - read_u16_be byte-order correctness.
//   - type_to_family mapping.
//   - to_ns_type mapping.
// =============================================================================

#include <cstdint>
#include <array>

#include <gtest/gtest.h>

#include "dns/util.hpp"
#include "dns_type.h"
#include "address_family.h"

// ── read_u16_be ──────────────────────────────────────────────────────────────

TEST(DnsUtilTest, ReadU16Be_Simple) {
    //        byte[0]=0x12  byte[1]=0x34  →  0x1234
    const std::uint8_t buf[2] = {0x12, 0x34};
    EXPECT_EQ(DNS::Util::read_u16_be(buf), 0x1234U);
}

TEST(DnsUtilTest, ReadU16Be_Zero) {
    const std::uint8_t buf[2] = {0x00, 0x00};
    EXPECT_EQ(DNS::Util::read_u16_be(buf), 0U);
}

TEST(DnsUtilTest, ReadU16Be_Max) {
    const std::uint8_t buf[2] = {0xFF, 0xFF};
    EXPECT_EQ(DNS::Util::read_u16_be(buf), 0xFFFFU);
}

TEST(DnsUtilTest, ReadU16Be_BigEndian_MSB) {
    // If bytes are {0x80, 0x00} → most-significant bit is set
    const std::uint8_t buf[2] = {0x80, 0x00};
    EXPECT_EQ(DNS::Util::read_u16_be(buf), 0x8000U);
}

TEST(DnsUtilTest, ReadU16Be_SpanOffset) {
    const std::array<uint8_t, 6> buf = {0x00, 0x00, 0x00, 0xAB, 0xCD, 0x00};
    std::span<const uint8_t> sp(buf);
    EXPECT_EQ(DNS::Util::read_u16_be(sp, 3), 0xABCDU);
}

// ── type_to_family ───────────────────────────────────────────────────────────

TEST(DnsUtilTest, TypeToFamily_A_ReturnsIPv4) {
    EXPECT_EQ(DNS::Util::type_to_family(DNS::Type::A), AddressFamily::IPV4);
}

TEST(DnsUtilTest, TypeToFamily_AAAA_ReturnsIPv6) {
    EXPECT_EQ(DNS::Util::type_to_family(DNS::Type::AAAA), AddressFamily::IPV6);
}

TEST(DnsUtilTest, TypeToFamily_TXT_ReturnsUnspecified) {
    EXPECT_EQ(DNS::Util::type_to_family(DNS::Type::TXT), AddressFamily::UNSPECIFIED);
}

TEST(DnsUtilTest, TypeToFamily_SOA_ReturnsUnspecified) {
    EXPECT_EQ(DNS::Util::type_to_family(DNS::Type::SOA), AddressFamily::UNSPECIFIED);
}

TEST(DnsUtilTest, TypeToFamily_Constexpr) {
    // Verify it can be evaluated at compile time.
    constexpr auto v4 = DNS::Util::type_to_family(DNS::Type::A);
    constexpr auto v6 = DNS::Util::type_to_family(DNS::Type::AAAA);
    static_assert(v4 == AddressFamily::IPV4);
    static_assert(v6 == AddressFamily::IPV6);
}

// ── to_ns_type ───────────────────────────────────────────────────────────────

TEST(DnsUtilTest, ToNsType_A_ReturnsNsTA) {
    EXPECT_EQ(DNS::Util::to_ns_type(DNS::Type::A), ns_t_a);
}

TEST(DnsUtilTest, ToNsType_AAAA_ReturnsNsTAAAA) {
    EXPECT_EQ(DNS::Util::to_ns_type(DNS::Type::AAAA), ns_t_aaaa);
}

TEST(DnsUtilTest, ToNsType_TXT_ReturnsNsTTXT) {
    EXPECT_EQ(DNS::Util::to_ns_type(DNS::Type::TXT), ns_t_txt);
}

TEST(DnsUtilTest, ToNsType_SOA_ReturnsNsTSOA) {
    EXPECT_EQ(DNS::Util::to_ns_type(DNS::Type::SOA), ns_t_soa);
}

TEST(DnsUtilTest, ToNsType_Constexpr) {
    constexpr auto a = DNS::Util::to_ns_type(DNS::Type::A);
    static_assert(a == ns_t_a);
}

TEST(DnsUtilTest, ToNsType_Default_ReturnsInvalid) {
    // A value not in the switch cases should fall through to the default branch.
    auto type = static_cast<DNS::Type>(42);
    EXPECT_EQ(DNS::Util::to_ns_type(type), ns_t_invalid);
}
