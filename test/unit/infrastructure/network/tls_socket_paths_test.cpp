//
// Unit tests for the socket primitives in src/infrastructure/network/transport/
// socket_ops.cpp that are decided by the descriptor alone: an empty interface
// name, a closed descriptor (-1), and a real socket bound to loopback.
//
// No descriptor here carries traffic. The failure cases use a closed
// descriptor, and the success cases create sockets and bind them to the
// loopback address; nothing is ever sent, connected or resolved.
//
// The CA-bundle cases cover the contract discover_ca_bundle() can be held to
// from any process: both entry points memoize their first result, and an engaged
// result names a readable file. The "no bundle anywhere" branches are the
// opposite — they need a host with no trust store and a cold process (see
// test/component/cert_util_test.cpp), so they are not reachable here.
//

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/network/tls/cert_util.h"
#include "infrastructure/network/transport/socket_ops.h"

namespace {

/// -1 is a closed descriptor: every socket option, bind and name lookup on it
/// fails with EBADF without touching the network.
constexpr int CLOSED_FD = -1;

[[nodiscard]] domain::InetAddress loopback_v4() {
    const auto address = domain::InetAddress::parse("127.0.0.1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// An unbound UDP socket of the requested family. Never connected and never
/// used to send: it exists so the option-setting helpers have a real
/// descriptor.
[[nodiscard]] std::expected<Utils::UniqueFd, net::IoError> open_udp_socket() {
    return net::detail::open_udp_socket(domain::AddressFamily::IPV4);
}

/// The interface that owns 127.0.0.1 — "lo" on Linux, "lo0" on the BSDs.
[[nodiscard]] std::string loopback_interface() {
    for (const auto& entry : ipsource::get_all()) {
        const bool owns_loopback = std::ranges::any_of(entry.addresses, [](const domain::InetAddress& address) {
            const auto* v4 = address.as_v4();
            return v4 != nullptr && v4->to_string() == "127.0.0.1";
        });
        if (owns_loopback) {
            return entry.name;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// bind_to_interface — SO_BINDTODEVICE, with an empty name short-circuiting it
// ---------------------------------------------------------------------------

TEST(SocketOpsPaths, BindToInterface_EmptyName_SucceedsWithoutUsingTheDescriptor) {
    // The "no interface configured" case must not reach setsockopt at all, so a
    // caller without the capability (or without a descriptor) still succeeds.
    const auto bound = net::detail::bind_to_interface(CLOSED_FD, "");

    EXPECT_TRUE(bound.has_value());
}

TEST(SocketOpsPaths, BindToInterface_ClosedDescriptor_Fails) {
    const auto bound = net::detail::bind_to_interface(CLOSED_FD, "lo");

    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(bound.error(), net::IoError::CONNECTION_FAILED);
}

TEST(SocketOpsPaths, BindToInterface_LiveSocket_BindsToTheNamedInterface) {
    const std::string loopback = loopback_interface();
    if (loopback.empty()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    auto socket = open_udp_socket();
    ASSERT_TRUE(socket.has_value());

    EXPECT_TRUE(net::detail::bind_to_interface(socket->get(), loopback).has_value());
}

// ---------------------------------------------------------------------------
// set_tcp_nodelay — best effort, and a failure is reported, never thrown
// ---------------------------------------------------------------------------

TEST(SocketOpsPaths, SetTcpNodelay_ClosedDescriptor_Fails) {
    const auto nodelay = net::detail::set_tcp_nodelay(CLOSED_FD);

    ASSERT_FALSE(nodelay.has_value());
    EXPECT_EQ(nodelay.error(), net::IoError::CONNECTION_FAILED);
}

// ---------------------------------------------------------------------------
// bind_local / local_port — the pair the UDP socket binds through
// ---------------------------------------------------------------------------

TEST(SocketOpsPaths, BindLocal_ClosedDescriptor_Fails) {
    const auto bound = net::detail::bind_local(CLOSED_FD, loopback_v4(), 0);

    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(bound.error(), net::IoError::CONNECTION_FAILED);
}

TEST(SocketOpsPaths, BindLocal_LoopbackSocket_BindsAnEphemeralPortThatLocalPortReports) {
    auto socket = open_udp_socket();
    ASSERT_TRUE(socket.has_value());

    ASSERT_TRUE(net::detail::bind_local(socket->get(), loopback_v4(), 0).has_value());
    const auto port = net::detail::local_port(socket->get());

    ASSERT_TRUE(port.has_value());
    EXPECT_NE(*port, 0);
}

TEST(SocketOpsPaths, LocalPort_ClosedDescriptor_Fails) {
    const auto port = net::detail::local_port(CLOSED_FD);

    ASSERT_FALSE(port.has_value());
    EXPECT_EQ(port.error(), net::IoError::CONNECTION_FAILED);
}

// ---------------------------------------------------------------------------
// set_socket_option — the multicast / IGMP / TTL shapes the transport helpers
// delegate here; a failure is a value, never an exception
// ---------------------------------------------------------------------------

TEST(SocketOpsPaths, SetSocketOption_ClosedDescriptor_Fails) {
    const int enabled = 1;

    const auto applied = net::detail::set_socket_option(CLOSED_FD, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    ASSERT_FALSE(applied.has_value());
    EXPECT_EQ(applied.error(), net::IoError::CONNECTION_FAILED);
}

TEST(SocketOpsPaths, SetSocketOption_LiveSocket_AcceptsABufferLengthOption) {
    auto socket = open_udp_socket();
    ASSERT_TRUE(socket.has_value());
    const int enabled = 1;

    EXPECT_TRUE(
        net::detail::set_socket_option(socket->get(), SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)).has_value());
}

// ---------------------------------------------------------------------------
// CA bundle discovery — memoised process-wide, so only the invariant is asserted
// ---------------------------------------------------------------------------

TEST(CertDiscoveryPaths, DiscoverCaBundle_RepeatedCalls_ReturnTheSameMemoisedResult) {
    const auto first = Utils::Cert::discover_ca_bundle();
    const auto second = Utils::Cert::discover_ca_bundle();

    EXPECT_EQ(second, first);
    if (first.has_value()) {
        EXPECT_FALSE(first->empty());
        std::error_code ec;
        EXPECT_TRUE(std::filesystem::is_regular_file(*first, ec)) << *first;
    }
}

TEST(CertDiscoveryPaths, GetSystemCaPath_RepeatedCalls_ReturnTheSameMemoisedResult) {
    const auto first = Utils::Cert::get_system_ca_path();
    const auto second = Utils::Cert::get_system_ca_path();

    EXPECT_EQ(second, first);
    if (first.has_value()) {
        EXPECT_FALSE(first->empty());
        std::error_code ec;
        EXPECT_TRUE(std::filesystem::is_regular_file(*first, ec)) << *first;
    }
}

}  // namespace