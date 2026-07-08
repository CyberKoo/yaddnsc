//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns_type.h — RecordKind enum and DnsServer struct.
//
// Verifies:
//   - All RecordKind enumerator values exist.
//   - DnsServer aggregate initialisation and defaults.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "dns_type.h"

// ── RecordKind ─────────────────────────────────────────────────────

TEST(RecordKindTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(RecordKind::A), 0);
    EXPECT_EQ(static_cast<int>(RecordKind::AAAA), 1);
    EXPECT_EQ(static_cast<int>(RecordKind::TXT), 2);
}

TEST(RecordKindTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<RecordKind>));
    EXPECT_FALSE((std::is_convertible_v<RecordKind, int>));
}

TEST(RecordKindTest, DefaultValue_IsA) {
    RecordKind t{};
    EXPECT_EQ(t, RecordKind::A);
}

// ── DnsServer ──────────────────────────────────────────────────────

TEST(DnsServerTest, DefaultPort_Is53) {
    DnsServer srv;
    EXPECT_EQ(srv.port, 53);
    EXPECT_TRUE(srv.address.empty());
}

TEST(DnsServerTest, AggregateInit) {
    DnsServer srv{.address = "1.1.1.1", .port = 853};
    EXPECT_EQ(srv.address, "1.1.1.1");
    EXPECT_EQ(srv.port, 853);
}

TEST(DnsServerTest, PartialAggregateInit) {
    DnsServer srv{.address = "8.8.8.8"};  // port defaults to 53
    EXPECT_EQ(srv.address, "8.8.8.8");
    EXPECT_EQ(srv.port, 53);
}

// DnsServer contains std::string, so it is NOT trivially copyable.
// This is expected and correct — std::string manages heap-allocated memory.
