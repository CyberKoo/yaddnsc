//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns_type.h — DNS::Type enum and DNS::Server struct.
//
// Verifies:
//   - All DNS::Type enumerator values exist.
//   - DNS::Server aggregate initialisation and defaults.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "dns_type.h"

// ── DNS::Type ────────────────────────────────────────────────────────────────

TEST(DnsTypeTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(DNS::Type::A), 0);
    EXPECT_EQ(static_cast<int>(DNS::Type::AAAA), 1);
    EXPECT_EQ(static_cast<int>(DNS::Type::TXT), 2);
    EXPECT_EQ(static_cast<int>(DNS::Type::SOA), 3);
}

TEST(DnsTypeTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<DNS::Type>));
    EXPECT_FALSE((std::is_convertible_v<DNS::Type, int>));
}

TEST(DnsTypeTest, DefaultValue_IsA) {
    DNS::Type t{};
    EXPECT_EQ(t, DNS::Type::A);
}

// ── DNS::Server ──────────────────────────────────────────────────────────────

TEST(DnsServerTest, DefaultPort_Is53) {
    DNS::Server srv;
    EXPECT_EQ(srv.port, 53);
    EXPECT_TRUE(srv.address.empty());
}

TEST(DnsServerTest, AggregateInit) {
    DNS::Server srv{.address = "1.1.1.1", .port = 853};
    EXPECT_EQ(srv.address, "1.1.1.1");
    EXPECT_EQ(srv.port, 853);
}

TEST(DnsServerTest, PartialAggregateInit) {
    DNS::Server srv{.address = "8.8.8.8"};  // port defaults to 53
    EXPECT_EQ(srv.address, "8.8.8.8");
    EXPECT_EQ(srv.port, 53);
}

// DNS::Server contains std::string, so it is NOT trivially copyable.
// This is expected and correct — std::string manages heap-allocated memory.

TEST(DnsServerTest, AddressFamily_A) {
    EXPECT_EQ(static_cast<int>(DNS::Type::A), 0);
}
