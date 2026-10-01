//
// Component tests for the mDNS code paths that the IPv4 happy-path suite in
// factory_mdns_test.cpp does not reach: the AAAA / IPv6 template
// instantiations, interface-selection fallbacks, and the pre-cancelled and
// malformed-hostname boundaries.
//
// Two deliberate constraints, both learned from the existing mDNS suite:
//
//  1. Every hostname is a random UUID per test. The integration sim server
//     catch-alls unknown A queries with 198.51.100.1, and a real responder on
//     the CI LAN (avahi-daemon, systemd-resolved) can answer too. Fixed
//     hostnames would therefore be answered and the assertions racy.
//
//  2. The A-record cases do join 224.0.0.251:5353, so this target must hold
//     the "mdns-multicast" RESOURCE_LOCK (see component/CMakeLists.txt) or it
//     races test_factory_mdns and integration_scenarios under `ctest -j`.
//
// No responder is started here. A UUID hostname is never answered, so
// resolve() reliably ends in a structured failure after the mDNS deadline,
// which is what these cases assert. Availability is probed up front because
// some CI runners (macOS in particular) have no multicast route: without the
// probe they would exercise the failure paths instead of the ones under test.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/network/net_devices.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "support/util/cancellation_token.hpp"
#include "support/util/random.hpp"

using IpSourceError = domain::IpSourceError;

namespace {

/// Random UUID v4 (e.g. "a1b2c3d4-e5f6-4789-abcd-ef1234567890") so no other
/// mDNS responder on the network can answer the query.
[[nodiscard]] std::string generate_uuid() {
    auto& eng = Utils::Random::engine();
    std::uniform_int_distribution<std::size_t> hex_dist(0, 15);
    std::uniform_int_distribution<std::size_t> variant_dist(0, 3);

    const char* hex_chars = "0123456789abcdef";
    std::string uuid(36, '\0');
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid[i] = '-';
        } else if (i == 14) {
            uuid[i] = '4';  // version 4
        } else if (i == 19) {
            uuid[i] = hex_chars[8 + variant_dist(eng)];  // variant 10xx
        } else {
            uuid[i] = hex_chars[hex_dist(eng)];
        }
    }
    return uuid;
}

/// A hostname no responder can answer.
[[nodiscard]] std::string unanswerable_hostname() {
    return generate_uuid() + ".local";
}

/// Quick probe for an IPv4 multicast route to 224.0.0.251:5353. macOS CI
/// runners often lack one, which makes sendto() fail with EHOSTUNREACH /
/// ENETUNREACH and every mDNS path degenerate into a setup failure.
[[nodiscard]] bool multicast_available() {
    Socket sock(AF_INET, SOCK_DGRAM);
    auto dest = SocketAddr::from_inet(Inet4Address::parse("224.0.0.251").value(), 5353);
    if (!dest) {
        return false;
    }
    const std::byte payload{0};
    return sock.send_to(std::span(&payload, 1), *dest) >= 0;
}

/// Probe the selected IPv6 interface, matching production's empty-name
/// default selection. Link-local multicast needs both an output IF and scope.
[[nodiscard]] bool multicast_available_v6(std::string_view interface) {
    Socket sock(AF_INET6, SOCK_DGRAM);
    const auto index = interface.empty() ? NetDevices::find_default_interface_index(AF_INET6).value_or(0)
                                         : NetDevices::name_to_index(std::string(interface));
    if (index == 0) {
        return false;
    }
    if (auto result = sock.set_option(IPPROTO_IPV6, IPV6_MULTICAST_IF, index); !result) {
        return false;
    }
    if (auto result = sock.set_option(IPPROTO_IPV6, IPV6_V6ONLY, 1); !result) {
        return false;
    }
    const auto bind_address = SocketAddr::from_inet(Inet6Address{}, 0).value();
    if (auto result = sock.bind(bind_address); !result) {
        return false;
    }
    ipv6_mreq membership{};
    ::inet_pton(AF_INET6, "ff02::fb", &membership.ipv6mr_multiaddr);
    membership.ipv6mr_interface = index;
    if (auto result = sock.set_option(IPPROTO_IPV6, IPV6_JOIN_GROUP, membership); !result) {
        return false;
    }
    // Closing the probe socket drops its membership even when send fails.
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(5353);
    inet_pton(AF_INET6, "ff02::fb", &address.sin6_addr);
    address.sin6_scope_id = index;
    const SocketAddr destination = SocketAddr::from_raw(reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const std::byte payload{0};
    return sock.send_to(std::span(&payload, 1), destination) >= 0;
}

#define SKIP_WITHOUT_IPV4_MULTICAST()                                      \
    do {                                                                   \
        if (!multicast_available()) {                                      \
            GTEST_SKIP() << "IPv4 multicast not available on this system"; \
        }                                                                  \
    } while (false)

#define SKIP_WITHOUT_IPV6_MULTICAST(interface)                             \
    do {                                                                   \
        if (!multicast_available_v6(interface)) {                          \
            GTEST_SKIP() << "IPv6 multicast not available on this system"; \
        }                                                                  \
    } while (false)

}  // namespace

// ===========================================================================
// AAAA — IPv6 multicast instantiations
// ===========================================================================

TEST(MdnsIpv6Test, AaaaRecord_ExercisesIpv6MulticastPath) {
    SKIP_WITHOUT_IPV6_MULTICAST("");
    // Nothing answers a UUID hostname, so the lookup ends in UNAVAILABLE after
    // the deadline. What matters is that the Ipv6Tag socket setup, group join
    // and interface resolution all ran.
    const MdnsIpSource source(unanswerable_hostname(), RecordKind::AAAA, "");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_NE(result.error().message.find("no valid response"), std::string::npos);
}

TEST(MdnsIpv6Test, AaaaRecord_WithLoopbackInterface_ExercisesExplicitIfIndex) {
    SKIP_WITHOUT_IPV6_MULTICAST(NetDevices::loopback_name());
    const MdnsIpSource source(unanswerable_hostname(), RecordKind::AAAA, NetDevices::loopback_name());
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_NE(result.error().message.find("no valid response"), std::string::npos);
}

TEST(MdnsIpv6Test, AaaaRecord_WithUnknownInterface_ReturnsUnavailable) {
    // A non-existent non-empty name yields index zero; production does not
    // enter the empty-interface default selection branch.
    const MdnsIpSource source(unanswerable_hostname(), RecordKind::AAAA, "definitely-not-an-interface");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}

// ===========================================================================
// Interface-selection fallbacks (IPv4)
// ===========================================================================

TEST(MdnsInterfaceFallbackTest, ARecord_WithUnknownInterface_FallsBackToInAddrAny) {
    SKIP_WITHOUT_IPV4_MULTICAST();
    // pick_ipv4_interface_addr() finds no subnet for the bogus name and falls
    // back to INADDR_ANY.
    const MdnsIpSource source(unanswerable_hostname(), RecordKind::A, "definitely-not-an-interface");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}

TEST(MdnsInterfaceFallbackTest, ARecord_WithEmptyInterface_UsesKernelRoutingTable) {
    SKIP_WITHOUT_IPV4_MULTICAST();
    const MdnsIpSource source(unanswerable_hostname(), RecordKind::A, "");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}

// ===========================================================================
// Cancellation
// ===========================================================================

TEST(MdnsCancellationTest, PreCancelledToken_ReturnsCancelled) {
    Utils::CancellationSource cancellation;
    cancellation.trigger();

    const MdnsIpSource source(unanswerable_hostname(), RecordKind::A, "");
    const auto result = source.resolve(cancellation.token());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::CANCELLED);
}

TEST(MdnsCancellationTest, PreCancelledToken_Aaaa_ReturnsCancelled) {
    Utils::CancellationSource cancellation;
    cancellation.trigger();

    const MdnsIpSource source(unanswerable_hostname(), RecordKind::AAAA, "");
    const auto result = source.resolve(cancellation.token());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::CANCELLED);
}

// ===========================================================================
// Malformed input — no socket is opened, so no multicast probe is needed.
// The source boundary must classify these as UNAVAILABLE rather than letting
// the query-builder exception escape.
// ===========================================================================

TEST(MdnsExceptionBoundaryTest, LabelLongerThan63Octets_ReturnsUnavailable) {
    const std::string bad_hostname = std::string(64, 'a') + ".local";
    const MdnsIpSource source(bad_hostname, RecordKind::A, "");

    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}

TEST(MdnsExceptionBoundaryTest, LabelLongerThan63Octets_Aaaa_ReturnsUnavailable) {
    const std::string bad_hostname = std::string(64, 'a') + ".local";
    const MdnsIpSource source(bad_hostname, RecordKind::AAAA, "");

    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}

TEST(MdnsExceptionBoundaryTest, SingleLabelWithoutDot_IsNotAnswered) {
    SKIP_WITHOUT_IPV4_MULTICAST();
    // A UUID label is still a valid single-label name, so the query goes out
    // and simply finds no responder.
    const MdnsIpSource source(generate_uuid(), RecordKind::A, "");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
}
