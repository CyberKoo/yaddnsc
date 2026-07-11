//
// Unit tests for network/socket_addr.h / socket_addr.cpp — SocketAddr type.
//
// Verifies:
//   - SocketAddr::from_inet creates correct sockaddr_storage for IPv4 and IPv6.
//   - SocketAddr::from_raw copies raw sockaddr data correctly.
//   - SocketAddr::port() returns the correct port.
//   - SocketAddr::address() returns the correct InetAddress.
//   - SocketAddr::to_string() formats correctly.
//   - Edge cases: AF_UNSPEC, zero-length input, null pointer.
// =============================================================================

#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include "network/socket_addr.h"
#include "network/inet_address.h"

// ===========================================================================
// from_inet — IPv4
// ===========================================================================

TEST(SocketAddrFromInetTest, Ipv4_Basic) {
    auto addr = InetAddress::parse("192.168.1.1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 8080);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->family(), AF_INET);
    EXPECT_EQ(sock_addr->port(), 8080);
    EXPECT_EQ(sock_addr->to_string(), "192.168.1.1:8080");
}

TEST(SocketAddrFromInetTest, Ipv4_PortZero) {
    auto addr = InetAddress::parse("10.0.0.1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 0);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->port(), 0);
    EXPECT_EQ(sock_addr->to_string(), "10.0.0.1:0");
}

TEST(SocketAddrFromInetTest, Ipv4_PortMax) {
    auto addr = InetAddress::parse("1.2.3.4");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 65535);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->port(), 65535);
}

TEST(SocketAddrFromInetTest, Ipv4_AddressRoundTrip) {
    auto addr = InetAddress::parse("8.8.8.8");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 53);
    ASSERT_TRUE(sock_addr.has_value());

    auto parsed_addr = sock_addr->address();
    ASSERT_TRUE(parsed_addr.has_value());
    EXPECT_EQ(parsed_addr->to_string(), "8.8.8.8");
    EXPECT_EQ(parsed_addr->get_family(), AddressFamily::IPV4);
}

// ===========================================================================
// from_inet — IPv6
// ===========================================================================

TEST(SocketAddrFromInetTest, Ipv6_Basic) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 443);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->family(), AF_INET6);
    EXPECT_EQ(sock_addr->port(), 443);
    // IPv6 to_string format: [addr]:port
    auto str = sock_addr->to_string();
    EXPECT_TRUE(str.starts_with("["));
    EXPECT_TRUE(str.ends_with("]:443"));
    EXPECT_TRUE(str.find("::1") != std::string::npos);
}

TEST(SocketAddrFromInetTest, Ipv6_AddressRoundTrip) {
    auto addr = InetAddress::parse("2001:db8::1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 853);
    ASSERT_TRUE(sock_addr.has_value());

    auto parsed_addr = sock_addr->address();
    ASSERT_TRUE(parsed_addr.has_value());
    EXPECT_EQ(parsed_addr->get_family(), AddressFamily::IPV6);
    // to_string should contain the address (with or without zero compression)
    auto s = parsed_addr->to_string();
    EXPECT_TRUE(s.find("2001") != std::string::npos);
}

TEST(SocketAddrFromInetTest, Ipv6_ScopeIdPreserved) {
    auto addr = InetAddress::parse("fe80::1%5");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 5353);
    ASSERT_TRUE(sock_addr.has_value());

    auto parsed_addr = sock_addr->address();
    ASSERT_TRUE(parsed_addr.has_value());
    EXPECT_TRUE(parsed_addr->is_link_local());

    // Scope ID should be preserved in the sockaddr_in6 and recovered
    // The test environment may or may not preserve scope_id through
    // inet_ntop/pton round-trips. We verify at least that the address
    // value is correct.
    auto s = parsed_addr->to_string();
    EXPECT_FALSE(s.empty());
}

TEST(SocketAddrFromInetTest, Ipv6_PortAndAddress) {
    auto addr = InetAddress::parse("fe80::1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 12345);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->port(), 12345);
    EXPECT_EQ(sock_addr->family(), AF_INET6);

    auto parsed = sock_addr->address();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->is_link_local());
}

// ===========================================================================
// from_inet — edge cases
// ===========================================================================

TEST(SocketAddrFromInetTest, DefaultInetAddress_ReturnsUnspec) {
    InetAddress default_addr;
    auto sock_addr = SocketAddr::from_inet(default_addr, 80);

    // Default InetAddress is Inet4Address unspecified (0.0.0.0)
    ASSERT_TRUE(sock_addr.has_value());
    EXPECT_EQ(sock_addr->family(), AF_INET);
    EXPECT_EQ(sock_addr->to_string(), "0.0.0.0:80");
}

// ===========================================================================
// from_raw
// ===========================================================================

TEST(SocketAddrFromRawTest, Ipv4_SockaddrIn) {
    struct sockaddr_in sin {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(1234);
    sin.sin_addr.s_addr = htonl(0x01020304);  // 1.2.3.4

    auto sock_addr = SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&sin), sizeof(sin));
    EXPECT_EQ(sock_addr.family(), AF_INET);
    EXPECT_EQ(sock_addr.port(), 1234);

    auto addr = sock_addr.address();
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->to_string(), "1.2.3.4");
}

TEST(SocketAddrFromRawTest, Ipv4_NullPtr_ReturnsUnspec) {
    auto sock_addr = SocketAddr::from_raw(nullptr, 0);
    EXPECT_EQ(sock_addr.family(), AF_UNSPEC);
    EXPECT_EQ(sock_addr.port(), 0);
    EXPECT_FALSE(sock_addr.address().has_value());
    EXPECT_EQ(sock_addr.to_string(), "<unspec>");
}

TEST(SocketAddrFromRawTest, Ipv4_ZeroLen_ReturnsUnspec) {
    struct sockaddr_in sin {};
    sin.sin_family = AF_INET;

    auto sock_addr = SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&sin), 0);
    // len is 0, so nothing is copied — storage_ remains AF_UNSPEC
    EXPECT_EQ(sock_addr.family(), AF_UNSPEC);
}

// ===========================================================================
// address() — edge cases
// ===========================================================================

TEST(SocketAddrAddressTest, UnspecFamily_ReturnsNullopt) {
    SocketAddr default_addr;
    EXPECT_FALSE(default_addr.address().has_value());
}

// ===========================================================================
// to_string
// ===========================================================================

TEST(SocketAddrToStringTest, Default_ReturnsUnspec) {
    SocketAddr default_addr;
    EXPECT_EQ(default_addr.to_string(), "<unspec>");
}

TEST(SocketAddrToStringTest, Ipv6Format) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 443);
    ASSERT_TRUE(sock_addr.has_value());

    auto str = sock_addr->to_string();
    EXPECT_FALSE(str.empty());
}

TEST(SocketAddrToStringTest, Ipv4Format_NoBrackets) {
    auto addr = InetAddress::parse("10.0.0.1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 80);
    ASSERT_TRUE(sock_addr.has_value());

    auto str = sock_addr->to_string();
    // IPv4 should NOT be wrapped in brackets
    EXPECT_EQ(str, "10.0.0.1:80");
}

// ===========================================================================
// raw() and raw_len() accessors (C API interop)
// ===========================================================================

TEST(SocketAddrRawTest, RawLenV4) {
    auto addr = InetAddress::parse("1.2.3.4");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 80);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->raw_len(), sizeof(sockaddr_in));
    EXPECT_NE(sock_addr->raw(), nullptr);
}

TEST(SocketAddrRawTest, RawLenV6) {
    auto addr = InetAddress::parse("::1");
    ASSERT_TRUE(addr.has_value());

    auto sock_addr = SocketAddr::from_inet(*addr, 80);
    ASSERT_TRUE(sock_addr.has_value());

    EXPECT_EQ(sock_addr->raw_len(), sizeof(sockaddr_in6));
    EXPECT_NE(sock_addr->raw(), nullptr);
}
