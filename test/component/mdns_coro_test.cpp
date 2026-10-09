//
// Component test for the coroutine mDNS IP source.
//
// A random-UUID `.local` name is never answered, so resolve() must end in a
// structured failure after the 500 ms response window rather than hanging. The
// IPv4 cases join 224.0.0.251:5353, so this target holds the "mdns-multicast"
// RESOURCE_LOCK (see component/CMakeLists.txt) exactly like the legacy mDNS
// suites.
//
// A UUID hostname (not a fixed one) keeps the assertion racy-free: the
// integration sim server answers unknown A queries, and an avahi-daemon on the
// CI LAN could answer a fixed name too.
//

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include <netinet/in.h>

#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/coro.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/network/transport/socket_ops.h"
#include "support/util/random.hpp"

using IpSourceError = domain::IpSourceError;

namespace {

using namespace std::chrono_literals;

/// Random UUID so no other mDNS responder on the network can answer the query.
[[nodiscard]] std::string unanswerable_hostname() {
    auto& engine = Utils::Random::engine();
    std::uniform_int_distribution<int> hex(0, 15);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string name;
    name.reserve(36 + 6);
    for (int i = 0; i < 36; ++i) {
        name.push_back(i == 8 || i == 13 || i == 18 || i == 23 ? '-' : digits[static_cast<std::size_t>(hex(engine))]);
    }
    name += ".local";
    return name;
}

/// Probe for a multicast route of @p family; a runner without one degenerates
/// that family's mDNS path into a fast setup failure, so the timing assertion
/// is skipped there. Mirrors the resolve path's observable setup — the group
/// join (its one hard configuration error) and the query send — because either
/// fails fast where the family is unroutable: FreeBSD, for example, rejects
/// joining a link-local group without an interface index.
[[nodiscard]] bool multicast_available(const domain::AddressFamily family) {
    auto socket = net::detail::open_udp_socket(family);
    if (!socket) {
        return false;
    }
    const bool ipv6 = family == domain::AddressFamily::IPV6;
    const auto group = domain::InetAddress::parse(ipv6 ? "ff02::fb" : "224.0.0.251");
    if (!group) {
        return false;
    }
    if (ipv6) {
        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group->as_v6()->data(), sizeof(membership.ipv6mr_multiaddr));
        membership.ipv6mr_interface = 0;
        if (!net::detail::set_socket_option(socket->get(), IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership,
                                            sizeof(membership))) {
            return false;
        }
    } else {
        ip_mreq membership{};
        std::memcpy(&membership.imr_multiaddr, group->as_v4()->data(), sizeof(membership.imr_multiaddr));
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!net::detail::set_socket_option(socket->get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                                            sizeof(membership))) {
            return false;
        }
    }
    coro::Loop loop;
    const std::uint8_t payload = 0;
    return coro::run(loop, net::detail::send_datagram(socket->get(), *group, 5353, std::span{&payload, 1}))
        .has_value();
}

/// One IPv4 or IPv6 lookup for an unanswerable name must fail, not hang.
void expect_structured_failure(const domain::RecordKind type) {
    const domain::AddressFamily family =
        type == domain::RecordKind::AAAA ? domain::AddressFamily::IPV6 : domain::AddressFamily::IPV4;
    const bool multicast = multicast_available(family);

    ipsource::MdnsIpSource source{unanswerable_hostname(), type, ""};
    coro::Loop loop;
    const auto start = std::chrono::steady_clock::now();
    const auto result = coro::run(loop, source.resolve());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    if (multicast) {
        // The query went out and the response window elapsed; it must not have
        // hung past it.
        EXPECT_GE(elapsed, 400ms);
    }
    EXPECT_LT(elapsed, 5s);
}

}  // namespace

TEST(MdnsCoro, UnanswerableHostnameFailsWithinTheWindow) {
    expect_structured_failure(domain::RecordKind::A);
}

TEST(MdnsCoro, UnanswerableIpv6HostnameFailsWithinTheWindow) {
    expect_structured_failure(domain::RecordKind::AAAA);
}
