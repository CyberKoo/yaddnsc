//
// Unit tests for src/domain/address_policy.h — domain::select_address.
//
// Locks the legacy Updater filtering rules:
//   - filtering applies only to AAAA records;
//   - link-local (fe80::/10) dropped unless allow_local_link;
//   - ULA (fc00::/7) dropped unless allow_ula;
//   - empty candidates (before or after filtering) → no address;
//   - the first surviving candidate wins.
// =============================================================================

#include "domain/address_policy.h"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"
#include "domain/network/inet_address.h"

namespace {

[[nodiscard]] domain::InetAddress v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return domain::InetAddress{domain::Inet4Address::from_bytes({a, b, c, d})};
}

[[nodiscard]] domain::InetAddress v6_global() {
    return domain::InetAddress{
        domain::Inet6Address::from_bytes({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

[[nodiscard]] domain::InetAddress v6_link_local() {
    return domain::InetAddress{
        domain::Inet6Address::from_bytes({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

[[nodiscard]] domain::InetAddress v6_ula() {
    return domain::InetAddress{
        domain::Inet6Address::from_bytes({0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

constexpr domain::AddressPolicy NONE_ALLOWED{false, false};
constexpr domain::AddressPolicy ALL_ALLOWED{true, true};

}  // namespace

// ── Empty input → no address ─────────────────────────────────────────────────

TEST(AddressPolicy, EmptyCandidates_NoAddress) {
    EXPECT_EQ(domain::select_address({}, domain::RecordKind::A, NONE_ALLOWED), std::nullopt);
    EXPECT_EQ(domain::select_address({}, domain::RecordKind::AAAA, ALL_ALLOWED), std::nullopt);
}

// ── A records: no filtering at all ───────────────────────────────────────────

TEST(AddressPolicy, ARecord_NoFiltering) {
    const std::vector<domain::InetAddress> candidates{v4(192, 0, 2, 1), v4(198, 51, 100, 1)};
    const auto picked = domain::select_address(candidates, domain::RecordKind::A, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "192.0.2.1");
}

// ── AAAA: link-local / ULA dropped by default ────────────────────────────────

TEST(AddressPolicy, Aaaa_FiltersLinkLocalWhenNotAllowed) {
    const auto picked = domain::select_address({v6_link_local(), v6_global()}, domain::RecordKind::AAAA, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

TEST(AddressPolicy, Aaaa_FiltersUlaWhenNotAllowed) {
    const auto picked = domain::select_address({v6_ula(), v6_global()}, domain::RecordKind::AAAA, NONE_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

TEST(AddressPolicy, Aaaa_AllFilteredOut_NoAddress) {
    EXPECT_EQ(domain::select_address({v6_link_local()}, domain::RecordKind::AAAA, NONE_ALLOWED), std::nullopt);
    EXPECT_EQ(domain::select_address({v6_ula()}, domain::RecordKind::AAAA, NONE_ALLOWED), std::nullopt);
}

// ── AAAA: kept when allowed ──────────────────────────────────────────────────

TEST(AddressPolicy, Aaaa_KeepsLinkLocalWhenAllowed) {
    const domain::AddressPolicy policy{.allow_ula = false, .allow_local_link = true};
    const auto picked = domain::select_address({v6_link_local()}, domain::RecordKind::AAAA, policy);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "fe80::1");
}

TEST(AddressPolicy, Aaaa_KeepsUlaWhenAllowed) {
    const domain::AddressPolicy policy{.allow_ula = true, .allow_local_link = false};
    const auto picked = domain::select_address({v6_ula()}, domain::RecordKind::AAAA, policy);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "fc00::1");
}

// ── First surviving candidate wins ───────────────────────────────────────────

TEST(AddressPolicy, Aaaa_FirstSurvivorWins) {
    const std::vector<domain::InetAddress> candidates{v6_global(), v6_link_local()};
    const auto picked = domain::select_address(candidates, domain::RecordKind::AAAA, ALL_ALLOWED);
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(picked->to_string(), "2001:db8::1");
}

TEST(AddressPolicy, SelectAddress_ARecordWithOnlyIpv6_ReturnsNoAddress) {
    EXPECT_FALSE(domain::select_address({v6_link_local()}, domain::RecordKind::A, NONE_ALLOWED).has_value());
}

TEST(AddressPolicy, SelectAddress_AaaaRecordWithOnlyIpv4_ReturnsNoAddress) {
    const auto ipv4 = domain::InetAddress::parse("203.0.113.1");
    ASSERT_TRUE(ipv4.has_value());
    EXPECT_FALSE(domain::select_address({*ipv4}, domain::RecordKind::AAAA, ALL_ALLOWED).has_value());
}

TEST(AddressPolicy, SelectAddress_MixedFamilies_SelectsMatchingRecordFamily) {
    const auto ipv4 = domain::InetAddress::parse("203.0.113.1");
    ASSERT_TRUE(ipv4.has_value());
    const auto a = domain::select_address({v6_global(), *ipv4}, domain::RecordKind::A, ALL_ALLOWED);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->to_string(), "203.0.113.1");
    const auto aaaa = domain::select_address({*ipv4, v6_global()}, domain::RecordKind::AAAA, ALL_ALLOWED);
    ASSERT_TRUE(aaaa.has_value());
    EXPECT_EQ(aaaa->to_string(), "2001:db8::1");
}
