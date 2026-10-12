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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <netinet/in.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/ip_source/mdns_receive.h"
#include "infrastructure/network/transport/socket_ops.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/util/fd.hpp"
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

namespace {

// Isolated unicast sockets exercise the production receive loop without
// depending on a multicast responder. Only the answer's source port is fixed.
class MdnsReceiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        address_ = domain::InetAddress::parse("127.0.0.1").value();
        auto opened = net::detail::open_udp_socket(domain::AddressFamily::IPV4);
        ASSERT_TRUE(opened.has_value());
        receiver_ = std::move(*opened);
        ASSERT_TRUE(net::detail::bind_local(receiver_.get(), address_, 0).has_value());
        const auto port = net::detail::local_port(receiver_.get());
        ASSERT_TRUE(port.has_value());
        port_ = *port;
        if (!sender_.bind(address_, 5353)) {
            GTEST_SKIP() << "Loopback mDNS source port unavailable";
        }
        ASSERT_TRUE(stray_sender_.bind(address_, 0).has_value());
        question_ =
            dns::QueryBuilder{0}.qr(true).aa(true).rd(false).add_question(hostname_, dns::RecordType::A).build();
        answer_ = question_;
        answer_[7] = 1;
        answer_.insert(answer_.end(), {0xc0, 0x0c, 0, 1, 0x80, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42});
    }

    coro::Task<void> send_burst() {
        const std::array<std::uint8_t, 1> malformed{0};
        for (unsigned i = 0; i < 64; ++i) {
            switch (i % 4) {
                case 0:
                    EXPECT_TRUE((co_await sender_.send_to(address_, port_, {})).has_value());
                    break;
                case 1:
                    EXPECT_TRUE((co_await sender_.send_to(address_, port_, malformed)).has_value());
                    break;
                case 2:
                    EXPECT_TRUE((co_await sender_.send_to(address_, port_, question_)).has_value());
                    break;
                default:
                    EXPECT_TRUE((co_await stray_sender_.send_to(address_, port_, answer_)).has_value());
                    break;
            }
        }
    }

    coro::Task<void> flood() {
        for (;;) {
            co_await send_burst();
            co_await coro::sleep_for(1ms);
        }
    }

    coro::Task<ipsource::Result> receive() {
        co_return co_await ipsource::detail::receive_mdns(receiver_.get(), hostname_, domain::RecordKind::A);
    }

    domain::InetAddress address_;
    Utils::UniqueFd receiver_;
    net::UdpSocket sender_;
    net::UdpSocket stray_sender_;
    std::uint16_t port_ = 0;
    const std::string hostname_ = "isolated-mdns.local";
    std::vector<std::uint8_t> question_;
    std::vector<std::uint8_t> answer_;
};

TEST_F(MdnsReceiveTest, MixedDiscards_BackOffAndAcceptTheLaterAnswer) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&]() -> coro::Task<void> {
        co_await send_burst();
        EXPECT_TRUE((co_await sender_.send_to(address_, port_, answer_)).has_value());
        const auto outcome = co_await coro::with_timeout(100ms, [this] { return receive(); });
        EXPECT_FALSE(outcome.timed_out);
        if (!outcome.has_value())
            co_return;
        EXPECT_TRUE(outcome->has_value());
        if (!*outcome)
            co_return;
        EXPECT_EQ((**outcome).size(), 1U);
        if (!(**outcome).empty()) {
            EXPECT_EQ((**outcome).front().to_string(), "192.0.2.42");
        }
    };
    coro::run(loop, task());
    // Two bursts of 32 across every discard path must each back off once.
    EXPECT_EQ(clock.now(), coro::TimePoint{2ms});
}

TEST_F(MdnsReceiveTest, Flood_DoesNotResetTheCallerWindow) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&]() -> coro::Task<void> {
        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(flood());
                const auto outcome = co_await coro::with_timeout(5ms, [this] { return receive(); });
                EXPECT_TRUE(outcome.timed_out);
                scope.cancel();
            });
        });
    };
    coro::run(loop, task());
    EXPECT_EQ(clock.now(), coro::TimePoint{5ms});
}

TEST_F(MdnsReceiveTest, Backoff_OuterCancellationPropagates) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool returned = false;
    auto waiting = [&]() -> coro::Task<void> {
        const auto result = co_await receive();
        returned = true;
        EXPECT_FALSE(result.has_value());
    };
    auto task = [&]() -> coro::Task<void> {
        co_await send_burst();
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(waiting());
                co_await coro::sleep_for(1ms);
                scope.cancel();
            });
        });
        EXPECT_TRUE(outcome.cancelled);
    };
    coro::run(loop, task());
    EXPECT_FALSE(returned);
    EXPECT_EQ(clock.now(), coro::TimePoint{1ms});
}

}  // namespace
