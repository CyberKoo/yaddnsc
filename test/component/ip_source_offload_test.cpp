// Isolated syscall rendezvous: blocks metadata, without sleeping or opening sockets.
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/iface.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/ip_source/result.h"

namespace {
std::atomic<bool> metadata_started{false};
std::atomic<bool> release_metadata{false};
std::atomic<bool> socket_opened{false};
ifaddrs* fake_snapshot = nullptr;
}  // namespace

extern "C" int getifaddrs(ifaddrs** ifap) noexcept(noexcept(::getifaddrs(ifap))) {
    metadata_started.store(true);
    release_metadata.wait(false);
    *ifap = fake_snapshot;
    return 0;
}

extern "C" void freeifaddrs(ifaddrs*) noexcept(noexcept(::freeifaddrs(nullptr))) {}

extern "C" unsigned int if_nametoindex(const char* name) noexcept(noexcept(::if_nametoindex(name))) {
    return std::string_view{name} == "eligible" ? 77U : 0U;
}

TEST(IpSourceOffload, BlockedMetadata_TimerCancelsAndSourceMayDieBeforeWorkerEnds) {
    using namespace std::chrono_literals;
    for (bool mdns : {false, true}) {
        metadata_started.store(false);
        release_metadata.store(false);
        coro::Loop loop;
        loop.set_offload_workers(1);
        auto root = [&]() -> coro::Task<void> {
            auto iface = std::make_unique<ipsource::InterfaceIpSource>("test-interface", domain::AddressFamily::IPV6);
            auto multicast = std::make_unique<ipsource::MdnsIpSource>("test.invalid", domain::RecordKind::AAAA, "");
            auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                    auto cancel = [&]() -> coro::Task<void> {
                        while (!metadata_started.load()) {
                            co_await coro::checkpoint();
                        }
                        co_await coro::sleep_for(20ms);
                        scope.cancel();
                    };
                    group.spawn_discard(cancel());
                    [[maybe_unused]] auto result = mdns ? co_await multicast->resolve() : co_await iface->resolve();
                    ADD_FAILURE() << "blocked metadata must observe the caller's cancellation";
                });
            });
            EXPECT_TRUE(outcome.cancelled);
            EXPECT_TRUE(metadata_started.load());
            // Only owned input packets remain on the worker after this point.
            iface.reset();
            multicast.reset();
            release_metadata.store(true);
            release_metadata.notify_one();
        };
        coro::run(loop, root());
    }
}

extern "C" int socket(int, int, int) noexcept(noexcept(::socket(0, 0, 0))) {
    socket_opened.store(true);
    errno = EPERM;
    return -1;
}

TEST(IpSourceOffload, DefaultIpv4_NoMetadata_DoesNotWaitForSaturatedPool) {
    using namespace std::chrono_literals;
    socket_opened.store(false);
    metadata_started.store(false);
    std::atomic<bool> worker_started{false};
    std::atomic<bool> release_worker{false};
    coro::Loop loop;
    loop.set_offload_workers(1);
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            auto blocker = [&]() -> coro::Task<void> {
                co_await coro::offload_joined([&] {
                    worker_started.store(true);
                    release_worker.wait(false);
                });
            };
            auto blocker_handle = group.spawn(blocker());
            while (!worker_started.load()) {
                co_await coro::checkpoint();
            }
            ipsource::MdnsIpSource source{"test.invalid", domain::RecordKind::A, ""};
            // Watchdog, not a latency bound: a resolve that wrongly queued behind the
            // saturated pool would hang forever (release_worker comes only after this
            // await), so any budget catches it; 2s absorbs CI scheduling jitter.
            auto outcome = co_await coro::with_timeout(
                2s, [&]() -> coro::Task<ipsource::Result> { co_return co_await source.resolve(); });
            EXPECT_FALSE(outcome.timed_out);
            EXPECT_TRUE(socket_opened.load());
            EXPECT_FALSE(metadata_started.load());
            release_worker.store(true);
            release_worker.notify_one();
            co_await blocker_handle;
        });
    };
    coro::run(loop, root());
}

TEST(IpSourceOffload, DefaultSelection_FiltersDownLoopbackPointToPointWrongFamilyAndMissingIndex) {
    release_metadata.store(true);
    std::array<ifaddrs, 7> entries{};
    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    std::array<char, 9> eligible{"eligible"};
    std::array<char, 8> missing{"missing"};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].ifa_next = i + 1 < entries.size() ? &entries[i + 1] : nullptr;
        entries[i].ifa_name = eligible.data();
        entries[i].ifa_flags = IFF_UP;
        entries[i].ifa_addr = reinterpret_cast<sockaddr*>(&ipv6);
    }
    entries[0].ifa_addr = nullptr;
    entries[1].ifa_addr = reinterpret_cast<sockaddr*>(&ipv4);
    entries[2].ifa_flags = 0;
    entries[3].ifa_flags = IFF_UP | IFF_LOOPBACK;
    entries[4].ifa_flags = IFF_UP | IFF_POINTOPOINT;
    entries[5].ifa_name = missing.data();
    fake_snapshot = entries.data();
    const auto index =
        coro::run(coro::offload([] { return ipsource::get_default_interface_index(domain::AddressFamily::IPV6); }));
    EXPECT_EQ(index, std::optional<unsigned int>{77});
    entries[6].ifa_flags = 0;
    EXPECT_FALSE(
        coro::run(coro::offload([] { return ipsource::get_default_interface_index(domain::AddressFamily::IPV6); })));
    fake_snapshot = nullptr;
}

TEST(IpSourceOffload, InterfaceSnapshot_PreservesIpv6ScopeIdAcrossWorkerBoundary) {
    release_metadata.store(true);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr.s6_addr[0] = 0xfe;
    address.sin6_addr.s6_addr[1] = 0x80;
    address.sin6_addr.s6_addr[15] = 1;
    address.sin6_scope_id = 77;
    std::array<char, 9> name{"eligible"};
    ifaddrs entry{};
    entry.ifa_name = name.data();
    entry.ifa_addr = reinterpret_cast<sockaddr*>(&address);
    fake_snapshot = &entry;
    ipsource::InterfaceIpSource source{"eligible", domain::AddressFamily::IPV6};
    const auto result = coro::run(source.resolve());
    fake_snapshot = nullptr;
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    ASSERT_NE(result->front().as_v6(), nullptr);
    EXPECT_EQ(result->front().as_v6()->get_scope_id(), 77U);
}

TEST(IpSourceOffload, StopSignal_CancelsBlockedMetadataBeforeWorkerDrain) {
    metadata_started.store(false);
    release_metadata.store(false);
    coro::StopSignals signals;
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        ipsource::InterfaceIpSource source{"eligible", domain::AddressFamily::IPV6};
        auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                auto stop = [&]() -> coro::Task<void> {
                    while (!metadata_started.load()) {
                        co_await coro::checkpoint();
                    }
                    EXPECT_EQ(std::raise(SIGTERM), 0);
                    co_await signals.wait();
                    scope.cancel();
                };
                group.spawn_discard(stop());
                [[maybe_unused]] auto result = co_await source.resolve();
                ADD_FAILURE() << "stop must cancel a blocked metadata wait";
            });
        });
        EXPECT_TRUE(outcome.cancelled);
        EXPECT_TRUE(signals.requested());
        EXPECT_FALSE(release_metadata.load());
        release_metadata.store(true);
        release_metadata.notify_one();
    };
    coro::run(loop, root());
}
