//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for network/inet_address.h / .cpp — IP address value types.
//
// Verifies:
//   - Inet4Address parsing, formatting, classification.
//   - Inet6Address parsing, formatting, classification (link-local, ULA, scope).
//   - InetAddress variant wrapper delegation.
//   - Edge cases: empty input, malformed addresses, boundary values.
// =============================================================================

#include <string_view>

#include <gtest/gtest.h>

#include "network/inet_address.h"

// ===========================================================================
// Inet4Address
// ===========================================================================

TEST(Inet4AddressTest, Parse_ValidIPv4) {
    auto addr = Inet4Address::parse("192.168.1.1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "192.168.1.1");
}

TEST(Inet4AddressTest, Parse_Loopback) {
    auto addr = Inet4Address::parse("127.0.0.1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_loopback());
}

TEST(Inet4AddressTest, Parse_Broadcast) {
    auto addr = Inet4Address::parse("255.255.255.255");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "255.255.255.255");
}

TEST(Inet4AddressTest, Parse_Multicast) {
    auto addr = Inet4Address::parse("224.0.0.1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_multicast());
}

TEST(Inet4AddressTest, Parse_Unspecified) {
    auto addr = Inet4Address::parse("0.0.0.0");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_unspecified());
}

TEST(Inet4AddressTest, Parse_Empty_ReturnsNullopt) {
    auto addr = Inet4Address::parse("");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet4AddressTest, Parse_InvalidFormat_ReturnsNullopt) {
    auto addr = Inet4Address::parse("not-an-ip");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet4AddressTest, Parse_OversizedOctet_ReturnsNullopt) {
    auto addr = Inet4Address::parse("192.168.1.256");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet4AddressTest, Parse_TooManyOctets_ReturnsNullopt) {
    auto addr = Inet4Address::parse("1.2.3.4.5");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet4AddressTest, FromBytes_RoundTrip) {
    Inet4Address::addr_type bytes = {10, 0, 0, 1};
    auto addr = Inet4Address::from_bytes(bytes);
    EXPECT_EQ(addr.to_string(), "10.0.0.1");
    EXPECT_EQ(addr.get_address(), bytes);
}

TEST(Inet4AddressTest, DefaultIsUnspecified) {
    Inet4Address addr;
    EXPECT_TRUE(addr.is_unspecified());
    EXPECT_EQ(addr.to_string(), "0.0.0.0");
}

TEST(Inet4AddressTest, Equality) {
    auto a1 = Inet4Address::parse("1.2.3.4");
    auto a2 = Inet4Address::parse("1.2.3.4");
    auto a3 = Inet4Address::parse("4.3.2.1");
    ASSERT_TRUE(a1.has_value());
    ASSERT_TRUE(a2.has_value());
    ASSERT_TRUE(a3.has_value());
    EXPECT_EQ(*a1, *a2);
    EXPECT_NE(*a1, *a3);
}

TEST(Inet4AddressTest, GetFamily) {
    EXPECT_EQ(Inet4Address::get_family(), AddressFamily::IPV4);
}

TEST(Inet4AddressTest, DataAccess) {
    auto addr = Inet4Address::parse("1.2.3.4");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->data()[0], 1);
    EXPECT_EQ(addr->data()[1], 2);
    EXPECT_EQ(addr->data()[2], 3);
    EXPECT_EQ(addr->data()[3], 4);
}

TEST(Inet4AddressTest, FromArray_DelegatesToFromBytes) {
    Inet4Address::addr_type bytes = {10, 0, 0, 1};
    auto addr = Inet4Address::from_array(bytes);
    EXPECT_EQ(addr.to_string(), "10.0.0.1");
    EXPECT_EQ(addr.get_address(), bytes);
}

TEST(Inet4AddressTest, AddrAccessor) {
    Inet4Address::addr_type bytes = {192, 168, 1, 1};
    auto addr = Inet4Address::from_bytes(bytes);
    EXPECT_EQ(addr.addr(), bytes);
}

// ===========================================================================
// Inet6Address
// ===========================================================================

TEST(Inet6AddressTest, Parse_Full) {
    auto addr = Inet6Address::parse("2001:db8::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_FALSE(addr->to_string().empty());
}

TEST(Inet6AddressTest, Parse_Loopback) {
    auto addr = Inet6Address::parse("::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_loopback());
}

TEST(Inet6AddressTest, Parse_Unspecified) {
    auto addr = Inet6Address::parse("::");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_unspecified());
}

TEST(Inet6AddressTest, Parse_Multicast) {
    auto addr = Inet6Address::parse("ff02::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_multicast());
}

TEST(Inet6AddressTest, Parse_LinkLocal) {
    auto addr = Inet6Address::parse("fe80::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_link_local());
    EXPECT_FALSE(addr->is_site_local());
    EXPECT_FALSE(addr->is_ula());
}

TEST(Inet6AddressTest, Parse_UniqueLocal) {
    auto addr = Inet6Address::parse("fc00::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_ula());
    EXPECT_FALSE(addr->is_link_local());
}

TEST(Inet6AddressTest, Parse_WithScopeId) {
    // Scope ID may be parsed differently on different platforms.
    auto addr = Inet6Address::parse("fe80::1%eth0");
    ASSERT_TRUE(addr.has_value());
    // On Linux, non-numeric scope IDs are silently ignored.
    // The address itself should still be valid.
    EXPECT_TRUE(addr->is_link_local());
}

TEST(Inet6AddressTest, Parse_Empty_ReturnsNullopt) {
    auto addr = Inet6Address::parse("");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet6AddressTest, Parse_Invalid_ReturnsNullopt) {
    auto addr = Inet6Address::parse("not-an-ip");
    EXPECT_FALSE(addr.has_value());
}

TEST(Inet6AddressTest, FromBytes_RoundTrip) {
    Inet6Address::addr_type bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 1};  // 2001:db8::1
    auto addr = Inet6Address::from_bytes(bytes);
    EXPECT_EQ(addr.get_address(), bytes);
}

TEST(Inet6AddressTest, Parse_WithNumericScopeId) {
    auto addr = Inet6Address::parse("fe80::1%2");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_link_local());
    EXPECT_EQ(addr->get_scope_id(), 2U);
}

TEST(Inet6AddressTest, Parse_WithEmptyScopeId) {
    // Trailing % with nothing after it — scope_str is empty
    auto addr = Inet6Address::parse("fe80::1%");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_link_local());
    EXPECT_EQ(addr->get_scope_id(), 0U);
}

TEST(Inet6AddressTest, Parse_WithNonNumericScopeId) {
    // Non-numeric scope ID (e.g. interface name) — from_chars fails, scope stays 0
    auto addr = Inet6Address::parse("fe80::1%eth0");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_link_local());
    EXPECT_EQ(addr->get_scope_id(), 0U);
}

TEST(Inet6AddressTest, ScopeId) {
    Inet6Address addr;
    addr.set_scope_id(42);
    EXPECT_EQ(addr.get_scope_id(), 42U);
}

TEST(Inet6AddressTest, ToString_WithScopeId) {
    Inet6Address addr;
    Inet6Address::addr_type bytes = {0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 2};
    addr = Inet6Address::from_bytes(bytes);
    addr.set_scope_id(5);
    auto s = addr.to_string();
    EXPECT_TRUE(s.find("%5") != std::string_view::npos);
}

TEST(Inet6AddressTest, FromArray_DelegatesToFromBytes) {
    Inet6Address::addr_type bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 1};
    auto addr = Inet6Address::from_array(bytes);
    EXPECT_EQ(addr.get_address(), bytes);
}

TEST(Inet6AddressTest, AddrAccessor) {
    Inet6Address::addr_type bytes = {0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 1};
    auto addr = Inet6Address::from_bytes(bytes);
    EXPECT_EQ(addr.addr(), bytes);
}

TEST(Inet6AddressTest, SiteLocal) {
    // fc00::/7 → fc00::1 is site-local (fc = 1111 1100, masked with 0xfe = 0xfc)
    auto addr = Inet6Address::parse("fec0::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_site_local());
    EXPECT_FALSE(addr->is_link_local());
    EXPECT_FALSE(addr->is_ula());
}

TEST(Inet6AddressTest, UniqueLocal_AlternatePrefix) {
    // fd00::/7 → fd00::1 is also ULA
    auto addr = Inet6Address::parse("fd00::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_ula());
}

TEST(Inet6AddressTest, DataAccess) {
    auto addr = Inet6Address::parse("::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->data()[15], 1);
    // All leading bytes should be 0 for ::1
    for (int i = 0; i < 15; ++i) {
        EXPECT_EQ(addr->data()[i], 0);
    }
}

TEST(Inet6AddressTest, DefaultIsUnspecified) {
    Inet6Address addr;
    EXPECT_TRUE(addr.is_unspecified());
}

TEST(Inet6AddressTest, Equality) {
    auto a1 = Inet6Address::parse("::1");
    auto a2 = Inet6Address::parse("::1");
    auto a3 = Inet6Address::parse("::2");
    ASSERT_TRUE(a1.has_value());
    ASSERT_TRUE(a2.has_value());
    ASSERT_TRUE(a3.has_value());
    EXPECT_EQ(*a1, *a2);
    EXPECT_NE(*a1, *a3);
}

TEST(Inet6AddressTest, GetFamily) {
    EXPECT_EQ(Inet6Address::get_family(), AddressFamily::IPV6);
}

// ===========================================================================
// InetAddress (variant wrapper)
// ===========================================================================

TEST(InetAddressTest, Parse_IPv4_ReturnsV4) {
    auto addr = InetAddress::parse("192.168.1.1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->get_family(), AddressFamily::IPV4);
    EXPECT_NE(addr->as_v4(), nullptr);
    EXPECT_EQ(addr->as_v6(), nullptr);
}

TEST(InetAddressTest, Parse_IPv6_ReturnsV6) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->get_family(), AddressFamily::IPV6);
    EXPECT_NE(addr->as_v6(), nullptr);
    EXPECT_EQ(addr->as_v4(), nullptr);
}

TEST(InetAddressTest, Parse_Empty_ReturnsNullopt) {
    auto addr = InetAddress::parse("");
    EXPECT_FALSE(addr.has_value());
}

TEST(InetAddressTest, Parse_Garbage_ReturnsNullopt) {
    auto addr = InetAddress::parse("clearly-invalid");
    EXPECT_FALSE(addr.has_value());
}

TEST(InetAddressTest, ToString_RoundTrip) {
    auto addr = InetAddress::parse("10.0.0.1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "10.0.0.1");
}

TEST(InetAddressTest, IPv6_ToString) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());
    // inet_ntop may produce "::1" or "0:0:0:0:0:0:0:1" depending on the OS.
    // Either is acceptable; just verify it's non-empty and looks like an IP.
    auto s = addr->to_string();
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find(':') != std::string_view::npos || s.find('.') != std::string_view::npos);
}

TEST(InetAddressTest, Classification_Delegation) {
    auto loopback = InetAddress::parse("127.0.0.1");
    auto multicast = InetAddress::parse("224.0.0.1");
    auto unspecified = InetAddress::parse("0.0.0.0");

    ASSERT_TRUE(loopback.has_value());
    ASSERT_TRUE(multicast.has_value());
    ASSERT_TRUE(unspecified.has_value());

    EXPECT_TRUE(loopback->is_loopback());
    EXPECT_FALSE(loopback->is_multicast());
    EXPECT_TRUE(multicast->is_multicast());
    EXPECT_TRUE(unspecified->is_unspecified());
}

TEST(InetAddressTest, IPv6Only_Classification) {
    auto link_local = InetAddress::parse("fe80::1");
    auto ula = InetAddress::parse("fc00::1");

    ASSERT_TRUE(link_local.has_value());
    ASSERT_TRUE(ula.has_value());

    EXPECT_TRUE(link_local->is_link_local());
    EXPECT_FALSE(link_local->is_ula());
    EXPECT_TRUE(ula->is_ula());
    EXPECT_FALSE(ula->is_link_local());
}

TEST(InetAddressTest, IPv4_IPv6Methods_ReturnFalse) {
    auto v4 = InetAddress::parse("1.2.3.4");
    ASSERT_TRUE(v4.has_value());
    EXPECT_FALSE(v4->is_link_local());
    EXPECT_FALSE(v4->is_site_local());
    EXPECT_FALSE(v4->is_ula());
    EXPECT_EQ(v4->get_scope_id(), 0U);
}

TEST(InetAddressTest, IPv6_SiteLocal) {
    auto addr = InetAddress::parse("fec0::1");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->is_site_local());
    EXPECT_FALSE(addr->is_link_local());
    EXPECT_FALSE(addr->is_ula());
}

TEST(InetAddressTest, IPv6_ScopeId) {
    auto addr = InetAddress::parse("fe80::1%10");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->get_scope_id(), 10U);
    EXPECT_TRUE(addr->is_link_local());
}

TEST(InetAddressTest, GetAddress_V4_ZeroPaddedTo16) {
    auto addr = InetAddress::parse("1.2.3.4");
    ASSERT_TRUE(addr.has_value());
    auto bytes = addr->get_address();
    // First 12 bytes are zero-padded (IPv4-in-IPv6 mapping? No, just zero-padded)
    EXPECT_EQ(bytes.size(), 16U);
    EXPECT_EQ(bytes[0], 1);
    EXPECT_EQ(bytes[1], 2);
    EXPECT_EQ(bytes[2], 3);
    EXPECT_EQ(bytes[3], 4);
    // Remaining bytes are zero-padded
    for (size_t i = 4; i < 16; ++i) {
        EXPECT_EQ(bytes[i], 0);
    }
}

TEST(InetAddressTest, GetAddress_V6) {
    auto addr = InetAddress::parse("2001:db8::1");
    ASSERT_TRUE(addr.has_value());
    auto bytes = addr->get_address();
    EXPECT_EQ(bytes.size(), 16U);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x01);
    EXPECT_EQ(bytes[2], 0x0d);
    EXPECT_EQ(bytes[3], 0xb8);
    EXPECT_EQ(bytes[15], 1);
}

TEST(InetAddressTest, Equality_CrossFamily) {
    auto v4 = InetAddress::parse("1.2.3.4");
    auto v6 = InetAddress::parse("::1");
    ASSERT_TRUE(v4.has_value());
    ASSERT_TRUE(v6.has_value());
    // An IPv4 and an IPv6 address should never be equal.
    EXPECT_NE(*v4, *v6);
}

TEST(InetAddressTest, FromBytes_V4) {
    std::array<uint8_t, 4> bytes = {8, 8, 8, 8};
    auto addr = InetAddress::from_bytes(std::span<const uint8_t>(bytes));
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->get_family(), AddressFamily::IPV4);
    EXPECT_EQ(addr->to_string(), "8.8.8.8");
}

TEST(InetAddressTest, FromBytes_V6) {
    std::array<uint8_t, 16> bytes = {};
    bytes[15] = 1;  // ::1
    auto addr = InetAddress::from_bytes(std::span<const uint8_t>(bytes));
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->get_family(), AddressFamily::IPV6);
}

TEST(InetAddressTest, FromBytes_InvalidLength) {
    std::array<uint8_t, 7> bytes = {};  // neither 4 nor 16
    auto addr = InetAddress::from_bytes(std::span<const uint8_t>(bytes));
    EXPECT_FALSE(addr.has_value());
}

TEST(InetAddressTest, DefaultIsV4Unspecified) {
    InetAddress addr;
    EXPECT_EQ(addr.get_family(), AddressFamily::IPV4);
    EXPECT_TRUE(addr.is_unspecified());
    EXPECT_EQ(addr.to_string(), "0.0.0.0");
}

TEST(InetAddressTest, Visit_IPv4) {
    auto addr = InetAddress::parse("10.0.0.1");
    ASSERT_TRUE(addr.has_value());
    bool visited_v4 = false;
    addr->visit([&](const auto &a) {
        visited_v4 = std::is_same_v<std::decay_t<decltype(a)>, Inet4Address>;
    });
    EXPECT_TRUE(visited_v4);
}

TEST(InetAddressTest, Visit_IPv6) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());
    bool visited_v6 = false;
    addr->visit([&](const auto &a) {
        visited_v6 = std::is_same_v<std::decay_t<decltype(a)>, Inet6Address>;
    });
    EXPECT_TRUE(visited_v6);
}

TEST(InetAddressTest, Visit_Mutable) {
    InetAddress addr{Inet4Address{}};
    addr.visit([](auto &a) {
        // Just verify the mutable overload compiles and runs
        using T = std::decay_t<decltype(a)>;
        EXPECT_TRUE((std::is_same_v<T, Inet4Address>));
    });
}
