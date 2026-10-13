//
// mDNS query — interface metadata and multicast socket configuration.
//
// mdns.cpp keeps every setup helper (interface metadata, multicast options,
// group join) in an anonymous namespace, so MdnsIpSource::resolve() is the only
// reachable entry. These cases drive that entry with a wrapped setsockopt that
// rejects chosen options: the group join is the source's one hard configuration
// error, so every case rejects it and the lookup ends before the query is bound
// or sent — no datagram leaves the host. What separates the cases is which
// options the source tries first, and that is what the assertions pin down.
//
// Requires "LINKER:--wrap=setsockopt" and YADDNSC_TEST_WRAP_SETSOCKOPT on the
// test target (see test/unit/CMakeLists.txt); without them the binary carries
// no case and the fault injection is not built.
//

#ifdef YADDNSC_TEST_WRAP_SETSOCKOPT

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/ip_source/result.h"
#include "infrastructure/network/transport/socket_ops.h"

namespace {

/// Options the injected setsockopt must reject; everything else reaches the
/// kernel untouched.
std::vector<std::pair<int, int>> rejected_options;
/// Every (level, option) pair the source attempted, in call order.
std::vector<std::pair<int, int>> attempted_options;

}  // namespace

extern "C" int __real_setsockopt(int fd, int level, int option, const void* value, socklen_t length);

extern "C" int __wrap_setsockopt(const int fd, const int level, const int option, const void* const value,
                                 const socklen_t length) {
    attempted_options.emplace_back(level, option);
    if (std::ranges::find(rejected_options, std::pair{level, option}) != rejected_options.end()) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return __real_setsockopt(fd, level, option, value, length);
}

namespace {

using IpSourceError = domain::IpSourceError;

/// Interface name no host has: the metadata walk must fall back to INADDR_ANY.
constexpr std::string_view ABSENT_INTERFACE = "yaddnsc-none0";

/// Never answered, and never sent to either — the lookup dies in socket setup.
constexpr std::string_view QUERIED_NAME = "yaddnsc-wrap-mdns.local";

/// One lookup on a fresh loop. The response window never opens: the rejected
/// group join ends the lookup first.
[[nodiscard]] ipsource::Result resolve(const std::string& interface, const domain::RecordKind type) {
    ipsource::MdnsIpSource source{std::string{QUERIED_NAME}, type, interface};
    return coro::run(source.resolve());
}

void reject(const int level, const int option) {
    rejected_options.emplace_back(level, option);
}

[[nodiscard]] bool attempted(const int level, const int option) {
    return std::ranges::find(attempted_options, std::pair{level, option}) != attempted_options.end();
}

/// The lookup failed as an unavailable source, not as an answer.
[[nodiscard]] bool failed_unavailable(const ipsource::Result& result) {
    return !result.has_value() && result.error().code == IpSourceError::Code::UNAVAILABLE;
}

[[nodiscard]] bool mentions(const ipsource::Result& result, const std::string_view fragment) {
    return !result.has_value() && result.error().message.find(fragment) != std::string::npos;
}

class MdnsSocketSetup : public ::testing::Test {
protected:
    void SetUp() override { reset(); }

    void TearDown() override { reset(); }

    static void reset() {
        rejected_options.clear();
        attempted_options.clear();
    }

    /// The interface that owns 127.0.0.1 — "lo" on Linux, "lo0" on the BSDs.
    /// Always up, always carrying an IPv4 address and an interface index.
    [[nodiscard]] static std::string loopback_interface() {
        for (const auto& entry : ipsource::get_all()) {
            const bool owns_loopback = std::ranges::any_of(entry.addresses, [](const domain::InetAddress& address) {
                const domain::Inet4Address* v4 = address.as_v4();
                return v4 != nullptr && v4->to_string() == "127.0.0.1";
            });
            if (owns_loopback) {
                return entry.name;
            }
        }
        return {};
    }

    /// The first interface whose addresses hold no IPv4 entry at all, or an
    /// empty string when the host has none. Its index is still known, so it
    /// separates "walked the addresses and found no IPv4" from "no index".
    [[nodiscard]] static std::string interface_without_ipv4() {
        for (const auto& entry : ipsource::get_all()) {
            const bool has_ipv4 = std::ranges::any_of(
                entry.addresses, [](const domain::InetAddress& address) { return address.as_v4() != nullptr; });
            if (!has_ipv4) {
                return entry.name;
            }
        }
        return {};
    }
};

TEST_F(MdnsSocketSetup, Ipv4_BestEffortOptionsFailAndTheGroupJoinEndsTheLookup) {
    const std::string loopback = loopback_interface();
    if (loopback.empty()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    reject(SOL_SOCKET, SO_REUSEPORT);
    reject(SOL_SOCKET, SO_BINDTODEVICE);
    reject(IPPROTO_IP, IP_MULTICAST_IF);
    reject(IPPROTO_IP, IP_MULTICAST_TTL);
    reject(IPPROTO_IP, IP_ADD_MEMBERSHIP);

    const ipsource::Result result = resolve(loopback, domain::RecordKind::A);

    EXPECT_TRUE(failed_unavailable(result));
    EXPECT_TRUE(mentions(result, "mDNS IP_ADD_MEMBERSHIP failed"));
    // SO_REUSEPORT, the device pin, the multicast output interface and the
    // TTL are best-effort: each one failed and the lookup still continued.
    EXPECT_TRUE(attempted(SOL_SOCKET, SO_REUSEPORT));
    EXPECT_TRUE(attempted(SOL_SOCKET, SO_BINDTODEVICE));
    EXPECT_TRUE(attempted(IPPROTO_IP, IP_MULTICAST_IF));
    EXPECT_TRUE(attempted(IPPROTO_IP, IP_MULTICAST_TTL));
    // The join is the last option attempted and the only one that may end it.
    const std::pair<int, int> last_attempt =
        attempted_options.empty() ? std::pair<int, int>{-1, -1} : attempted_options.back();
    EXPECT_EQ(last_attempt, (std::pair{IPPROTO_IP, IP_ADD_MEMBERSHIP}));
    // Nothing was bound or sent afterwards.
    EXPECT_FALSE(attempted(SOL_SOCKET, SO_REUSEADDR));
}

TEST_F(MdnsSocketSetup, UnknownInterface_FallsBackToAnyAddressAndPinsTheAbsentName) {
    if (ipsource::get_addresses(std::string{ABSENT_INTERFACE}).has_value()) {
        GTEST_SKIP() << "the probe interface name is in use";
    }
    reject(IPPROTO_IP, IP_ADD_MEMBERSHIP);

    const ipsource::Result result = resolve(std::string{ABSENT_INTERFACE}, domain::RecordKind::A);

    EXPECT_TRUE(failed_unavailable(result));
    EXPECT_TRUE(mentions(result, "mDNS IP_ADD_MEMBERSHIP failed"));
    // The absent name is still handed to the device pin — best-effort, so the
    // lookup carries on to the TTL and the join.
    EXPECT_TRUE(attempted(SOL_SOCKET, SO_BINDTODEVICE));
    EXPECT_TRUE(attempted(IPPROTO_IP, IP_MULTICAST_TTL));
    // Without an index the multicast output interface is the kernel's choice.
    EXPECT_FALSE(attempted(IPPROTO_IP, IP_MULTICAST_IF));
    EXPECT_FALSE(attempted(SOL_SOCKET, SO_REUSEADDR));
}

TEST_F(MdnsSocketSetup, Ipv6_JoinFailureIsReportedAfterTheBestEffortMulticastOptions) {
    if (!net::detail::open_udp_socket(domain::AddressFamily::IPV6).has_value()) {
        GTEST_SKIP() << "IPv6 sockets unavailable";
    }
    const std::string loopback = loopback_interface();
    if (loopback.empty()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    reject(IPPROTO_IPV6, IPV6_V6ONLY);
    reject(IPPROTO_IPV6, IPV6_MULTICAST_IF);
    reject(IPPROTO_IPV6, IPV6_MULTICAST_HOPS);
    reject(IPPROTO_IPV6, IPV6_JOIN_GROUP);

    const ipsource::Result result = resolve(loopback, domain::RecordKind::AAAA);

    EXPECT_TRUE(failed_unavailable(result));
    EXPECT_TRUE(mentions(result, "mDNS IPV6_JOIN_GROUP failed"));
    EXPECT_TRUE(attempted(IPPROTO_IPV6, IPV6_V6ONLY));
    // A known index pins the multicast output interface and the link-local join.
    EXPECT_TRUE(attempted(IPPROTO_IPV6, IPV6_MULTICAST_IF));
    EXPECT_TRUE(attempted(IPPROTO_IPV6, IPV6_MULTICAST_HOPS));
    // The IPv4 group is never joined on the AAAA path.
    EXPECT_FALSE(attempted(IPPROTO_IP, IP_ADD_MEMBERSHIP));
    EXPECT_FALSE(attempted(SOL_SOCKET, SO_REUSEADDR));
}

TEST_F(MdnsSocketSetup, InterfaceWithoutIpv4_KeepsTheIndexAndFallsBackToAnyAddress) {
    const std::string interface = interface_without_ipv4();
    if (interface.empty()) {
        GTEST_SKIP() << "no interface carries only non-IPv4 addresses";
    }
    reject(IPPROTO_IP, IP_ADD_MEMBERSHIP);

    const ipsource::Result result = resolve(interface, domain::RecordKind::A);

    EXPECT_TRUE(failed_unavailable(result));
    EXPECT_TRUE(mentions(result, "mDNS IP_ADD_MEMBERSHIP failed"));
    // The address walk ran to its end without an IPv4 entry, but the index
    // still pinned the multicast output interface.
    EXPECT_TRUE(attempted(IPPROTO_IP, IP_MULTICAST_IF));
}

}  // namespace

#endif  // YADDNSC_TEST_WRAP_SETSOCKOPT