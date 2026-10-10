// Unit tests for record_kind.h and domain::DnsServer.
//
// Verifies:
//   - All RecordKind enumerator values exist.
//   - DnsServer aggregate initialisation and defaults.
// =============================================================================

#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"

// ── RecordKind ─────────────────────────────────────────────────────

TEST(RecordKindTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(domain::RecordKind::A), 0);
    EXPECT_EQ(static_cast<int>(domain::RecordKind::AAAA), 1);
    EXPECT_EQ(static_cast<int>(domain::RecordKind::TXT), 2);
}

TEST(RecordKindTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<domain::RecordKind>) );
    EXPECT_FALSE((std::is_convertible_v<domain::RecordKind, int>) );
}

TEST(RecordKindTest, DefaultValue_IsA) {
    domain::RecordKind t{};
    EXPECT_EQ(t, domain::RecordKind::A);
}

// ── DnsServer ──────────────────────────────────────────────────────

TEST(DnsServerTest, DefaultPort_Is53) {
    domain::DnsServer srv;
    EXPECT_EQ(srv.port, 53);
    EXPECT_TRUE(srv.address.empty());
}

TEST(DnsServerTest, AggregateInit) {
    domain::DnsServer srv{.address = "1.1.1.1", .port = 853};
    EXPECT_EQ(srv.address, "1.1.1.1");
    EXPECT_EQ(srv.port, 853);
}

TEST(DnsServerTest, PartialAggregateInit) {
    domain::DnsServer srv{.address = "8.8.8.8"};  // port defaults to 53
    EXPECT_EQ(srv.address, "8.8.8.8");
    EXPECT_EQ(srv.port, 53);
}

// DnsServer contains std::string, so it is NOT trivially copyable.
// This is expected and correct — std::string manages heap-allocated memory.
