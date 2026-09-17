//
// Unit tests for src/domain/address_policy.h — domain::select_address.
//
// Locks the legacy Updater filtering rules (Phase 0):
//   - filtering applies only to AAAA records;
//   - link-local (fe80::/10) dropped unless allow_local_link;
//   - ULA (fc00::/7) dropped unless allow_ula;
//   - empty candidates (before or after filtering) → no address;
//   - the first surviving candidate wins.
// =============================================================================

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "domain/address_policy.h"

namespace {

[[nodiscard]] InetAddress v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return InetAddress{Inet4Address::from_bytes({a, b, c, d})};
}

[[nodiscard]] InetAddress v6_global() {
    return InetAddress{Inet6Address::from_bytes({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

[[nodiscard]] InetAddress v6_link_local() {
    return InetAddress{Inet6Address::from_bytes({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

[[nodiscard]] InetAddress v6_ula() {
    return InetAddress{Inet6Address::from_bytes({0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

constexpr domain::AddressPolicy NONE_ALLOWED{false, false};
constexpr domain::AddressPolicy ALL_ALLOWED{true, true};

} // namespace

// ── Empty input → no address ─────────────────────────────────────────────────

TEST(AddressPolicy, EmptyCandidates_NoAddress) {
    EXPECT_EQ(domain::select_address({}, RecordKind::A, NONE_ALLOWED), std::nullopt);
    EXPECT_EQ(domain::select_address({}, RecordKind::AAAA, ALL_ALLOWED), std::nullopt);
}

// ── A records: no filtering at all ───────────────────────────────────────────

TEST(AddressPolicy, ARecord_NoFiltering) {
    const std::vector<InetAddress> candidates{v4(192, 0, 2, 1), v4(198, 51, 100, 1)};
    const auto picked = domain::select_address(candidates, RecordKind::A, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "192.0.2.1");
}

// ── AAAA: link-local / ULA dropped by default ────────────────────────────────

TEST(AddressPolicy, Aaaa_FiltersLinkLocalWhenNotAllowed) {
    const auto picked = domain::select_address({v6_link_local(), v6_global()}, RecordKind::AAAA, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

TEST(AddressPolicy, Aaaa_FiltersUlaWhenNotAllowed) {
    const auto picked = domain::select_address({v6_ula(), v6_global()}, RecordKind::AAAA, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

TEST(AddressPolicy, Aaaa_AllFilteredOut_NoAddress) {
    EXPECT_EQ(domain::select_address({v6_link_local()}, RecordKind::AAAA, NONE_ALLOWED), std::nullopt);
    EXPECT_EQ(domain::select_address({v6_ula()}, RecordKind::AAAA, NONE_ALLOWED), std::nullopt);
}

// ── AAAA: kept when allowed ──────────────────────────────────────────────────

TEST(AddressPolicy, Aaaa_KeepsLinkLocalWhenAllowed) {
    const domain::AddressPolicy policy{.allow_ula = false, .allow_local_link = true};
    const auto picked = domain::select_address({v6_link_local()}, RecordKind::AAAA, policy);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "fe80::1");
}

TEST(AddressPolicy, Aaaa_KeepsUlaWhenAllowed) {
    const domain::AddressPolicy policy{.allow_ula = true, .allow_local_link = false};
    const auto picked = domain::select_address({v6_ula()}, RecordKind::AAAA, policy);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "fc00::1");
}

// ── First surviving candidate wins ───────────────────────────────────────────

TEST(AddressPolicy, Aaaa_FirstSurvivorWins) {
    const std::vector<InetAddress> candidates{v6_global(), v6_link_local()};
    const auto picked = domain::select_address(candidates, RecordKind::AAAA, ALL_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

// ── A records never filter, even with IPv6-looking content in the list ───────

TEST(AddressPolicy, ARecord_NeverFiltersV6Candidates) {
    // An A task handed only IPv6 candidates keeps them (family filtering is
    // the IP source's job via ip_type, not the address policy's).
    const auto picked = domain::select_address({v6_link_local()}, RecordKind::A, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "fe80::1");
}
