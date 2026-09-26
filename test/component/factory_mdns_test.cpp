//
// Integration tests for IpSourceFactory and MdnsIpSource.
//
// Factory test: creates an INTERFACE source via the factory.
// mDNS test:   sets up a local multicast responder on loopback,
//              resolves via MdnsIpSource, and verifies the result.
//
// These tests link the DNS + IP-source dependency chain directly
// (socket, net_devices, builder, parser, etc.).
//
// Cancellation: one test arms the responder to hold its reply after
// acknowledging the query, then triggers the token while MdnsIpSource waits —
// resolve() must return CANCELLED early instead of waiting out the deadline.
// =============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/base.h"
#include "infrastructure/ip_source/factory.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/network/net_devices.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "support/util/cancellation_token.hpp"
#include "support/util/random.hpp"

namespace {
const std::string LOOPBACK = NetDevices::loopback_name();

/// Generate a random UUID v4 string (e.g. "a1b2c3d4-e5f6-4789-abcd-ef1234567890").
/// Each call produces a unique hostname, ensuring mDNS queries from this test
/// are not answered by other mDNS responders on the network (e.g. avahi-daemon,
/// systemd-resolved).
[[nodiscard]] std::string generate_uuid() {
    auto& eng = Utils::Random::engine();
    std::uniform_int_distribution<std::size_t> hex_dist(0, 15);
    std::uniform_int_distribution<std::size_t> variant_dist(0, 3);

    const char* hex_chars = "0123456789abcdef";
    // UUID format: 8-4-4-4-12 = 36 chars
    std::string uuid(36, '\0');
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid[i] = '-';
        } else if (i == 14) {
            uuid[i] = '4';  // Version 4
        } else if (i == 19) {
            // Variant: 10xx -> 8, 9, a, or b
            uuid[i] = hex_chars[8 + variant_dist(eng)];
        } else {
            uuid[i] = hex_chars[hex_dist(eng)];
        }
    }
    return uuid;
}

/// Quick probe to check whether UDP multicast sending to 224.0.0.251:5353
/// works on this system.  macOS CI runners often lack a multicast route,
/// causing sendto() to fail with EHOSTUNREACH / ENETUNREACH.
[[nodiscard]] bool multicast_available() {
    Socket sock(AF_INET, SOCK_DGRAM);
    auto dest = SocketAddr::from_inet(Inet4Address::parse("224.0.0.251").value(), 5353);
    if (!dest) {
        return false;
    }
    std::byte payload{0};
    auto ret = sock.send_to(std::span(&payload, 1), *dest);
    if (ret >= 0) {
        return true;
    }
    // ENETUNREACH / EHOSTUNREACH are the expected failures when there is no
    // multicast route.  Any other error is unexpected but we treat it as
    // "not available" to stay safe.
    return false;
}
}  // anonymous namespace

using namespace std::chrono_literals;

// ===========================================================================
// IpSourceFactory — create InterfaceIpSource
// ===========================================================================

TEST(IpSourceFactoryTest, CreateInterfaceSource_ResolvesLoopback) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;

    // resolve() must work using the real loopback interface.
    const auto addrs = (*source)->resolve({});
    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    EXPECT_FALSE(addrs->empty());
    EXPECT_TRUE(std::ranges::any_of(*addrs, [](const InetAddress& a) { return a.to_string() == "127.0.0.1"; }));
}

TEST(IpSourceFactoryTest, CreateInterfaceSource_Ipv6) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::AAAA;
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;
    const auto addrs = (*source)->resolve({});
    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;

    if (addrs->empty()) {
        GTEST_SKIP() << "IPv6 is not available on this system";
    }
    for (const auto& addr : *addrs) {
        EXPECT_EQ(addr.get_family(), AddressFamily::IPV6);
    }
}

// ===========================================================================
// IpSourceFactory — create HttpIpSource (constructor only, no I/O)
// ===========================================================================

TEST(IpSourceFactoryTest, CreateHttpSource_ConstructsSuccessfully) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::HTTP;
    cfg.ip_source_param = "http://127.0.0.1:1/ip";  // valid URL, no server needed for construction

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;
    // Constructor succeeds — resolves via PersistentHttpClient.
    // resolve() would fail with connection refused, which is expected.
}

TEST(IpSourceFactoryTest, CreateHttpSource_WithIface_BindsToInterface) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::HTTP;
    cfg.ip_source_param = "http://127.0.0.1:1/ip";
    cfg.interface = LOOPBACK;

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;
}

// ===========================================================================
// IpSourceFactory — type_to_family with unknown record type
// ===========================================================================

TEST(IpSourceFactoryTest, UnknownType_FallsBackToUnspecified) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::TXT;  // not A or AAAA → UNSPECIFIED
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;

    // UNSPECIFIED returns all addresses on the interface.
    const auto addrs = (*source)->resolve({});
    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    EXPECT_FALSE(addrs->empty());
}

// ===========================================================================
// mDNS — local multicast responder
//
// Starts a UDP listener on 224.0.0.251:5353 (joined on loopback).
// When it receives a DNS query, it responds with a crafted A record.
// The MdnsIpSource resolves the hostname via mDNS and should get
// the IP we sent back.
//
// Uses a random UUID hostname per test run to prevent other mDNS
// responders (avahi-daemon, systemd-resolved) from answering our queries.
// ===========================================================================

class MdnsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // ---- Check multicast availability ----------------------------------
        if (!multicast_available()) {
            GTEST_SKIP() << "mDNS multicast not available on this system";
        }

        // ---- Generate unique hostname per test run -------------------------
        // A random UUID prevents other mDNS responders from answering our
        // query and causing flaky failures.
        test_hostname_ = generate_uuid() + ".local";

        // ---- Create responder socket ---------------------------------------
        responder_sock_ = std::make_unique<Socket>(AF_INET, SOCK_DGRAM);
        responder_sock_->set_reuseaddr(true).value();
        responder_sock_->set_option(SOL_SOCKET, SO_REUSEPORT, 1).value();

        auto bind_addr = SocketAddr::from_inet(Inet4Address{}, 5353);
        ASSERT_TRUE(bind_addr.has_value());
        responder_sock_->bind(*bind_addr).value();

        // Join multicast group 224.0.0.251 on the default interface.
        auto mcast_addr = Inet4Address::parse("224.0.0.251");
        ASSERT_TRUE(mcast_addr.has_value());

        // Build ip_mreq: group address + INADDR_ANY interface.
        mreq_ = ip_mreq{};
        auto* dest = reinterpret_cast<std::uint8_t*>(&mreq_.imr_multiaddr);
        std::copy_n(mcast_addr->data(), sizeof(mreq_.imr_multiaddr), dest);
        mreq_.imr_interface.s_addr = INADDR_ANY;
        responder_sock_->set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_).value();

        // ---- Start responder thread ----------------------------------------
        stop_flag_.store(false);
        responder_thread_ = std::thread([this] { responder_loop(); });

        // Give the responder a moment to start.
        std::this_thread::sleep_for(20ms);
    }

    void TearDown() override {
        stop_flag_.store(true);
        if (responder_thread_.joinable()) {
            responder_thread_.join();
        }
        if (forger_thread_.joinable()) {
            forger_thread_.join();
        }
        if (responder_sock_) {
            // Leave multicast group.
            (void) responder_sock_->set_option(IPPROTO_IP, IP_DROP_MEMBERSHIP, mreq_);
            responder_sock_->close();
        }
        if (forger_sock_) {
            (void) forger_sock_->set_option(IPPROTO_IP, IP_DROP_MEMBERSHIP, mreq_);
            forger_sock_->close();
        }
        responder_sock_.reset();
        forger_sock_.reset();
    }

    /// Start a second responder that answers from a NON-5353 source port
    /// (an attacker-style forged reply).  Must be called from the test body
    /// after SetUp.
    void start_forger() {
        forger_sock_ = std::make_unique<Socket>(AF_INET, SOCK_DGRAM);
        forger_sock_->set_reuseaddr(true).value();
        forger_sock_->set_option(SOL_SOCKET, SO_REUSEPORT, 1).value();

        // Ephemeral port — deliberately NOT 5353 (RFC 6762 §6 violation).
        auto forger_bind = SocketAddr::from_inet(Inet4Address{}, 0);
        ASSERT_TRUE(forger_bind.has_value());
        forger_sock_->bind(*forger_bind).value();

        forger_sock_->set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_).value();
        forger_thread_ = std::thread([this] { forger_loop(); });
    }

    /// Check how many matching queries the responder received.
    [[nodiscard]] int query_count() const { return query_count_.load(); }

    /// Let a responder parked by hold_response_ send its reply and exit.
    void release_response() {
        {
            std::lock_guard lock(hold_mtx_);
            respond_release_ = true;
        }
        hold_cv_.notify_all();
    }

    std::string test_hostname_;  // Random UUID hostname for this test run

    /// When true, the responder appends an unrelated A record to its reply.
    std::atomic<bool> include_unrelated_record_{false};

    /// When true, the responder sends a garbage datagram (from the correct
    /// port 5353) before the genuine reply — the client must tolerate and
    /// discard it instead of aborting the whole query.
    std::atomic<bool> send_garbage_first_{false};

    /// When true, the responder signals query_received_ on a matching query
    /// and parks before replying until release_response() (or TearDown).
    std::atomic<bool> hold_response_{false};
    std::promise<void> query_received_;
    std::atomic<bool> query_signalled_{false};

private:
    /// Encode a dot-separated hostname into DNS label format
    /// (e.g. "foo.local" -> "\x03foo\x05local\x00").
    [[nodiscard]] static std::vector<std::uint8_t> encode_dns_name(std::string_view name) {
        std::vector<std::uint8_t> encoded;
        size_t start = 0;
        while (start < name.size()) {
            auto dot = name.find('.', start);
            auto len = (dot == std::string_view::npos) ? name.size() - start : dot - start;
            encoded.push_back(static_cast<std::uint8_t>(len));
            for (size_t i = 0; i < len; ++i) {
                encoded.push_back(static_cast<std::uint8_t>(name[start + i]));
            }
            start = (dot == std::string_view::npos) ? name.size() : dot + 1;
        }
        encoded.push_back(0);  // root label
        return encoded;
    }

    /// Check whether the QNAME in a DNS query (starting at offset 12) matches
    /// the given encoded hostname.
    [[nodiscard]] static bool qname_matches(std::span<const std::uint8_t> query,
                                            const std::vector<std::uint8_t>& encoded_hostname) {
        if (query.size() < 12 + encoded_hostname.size()) {
            return false;
        }
        return std::ranges::equal(encoded_hostname, query.subspan(12, encoded_hostname.size()));
    }

    /// Build a crafted mDNS A-record response echoing the query's TXID and
    /// question section.  Optionally appends an unrelated A record to test
    /// owner-name filtering.
    [[nodiscard]] std::vector<std::uint8_t> build_response(std::span<const std::uint8_t> query,
                                                           std::uint8_t a,
                                                           std::uint8_t b,
                                                           std::uint8_t c,
                                                           std::uint8_t d,
                                                           bool include_unrelated = false) const {
        std::vector<std::uint8_t> resp;
        resp.reserve(query.size() + 48);

        // Header (copies query's TXID): QR=1, RA=1, QDCOUNT=1, ANCOUNT=1(+1).
        resp.push_back(query[0]);
        resp.push_back(query[1]);
        resp.push_back(0x80);
        resp.push_back(0x80);
        resp.push_back(0x00);
        resp.push_back(0x01);
        resp.push_back(0x00);
        resp.push_back(include_unrelated ? 0x02 : 0x01);
        resp.push_back(0x00);
        resp.push_back(0x00);
        resp.push_back(0x00);
        resp.push_back(0x00);

        // Echo back the question section from the query (starts at offset 12).
        size_t qname_end = 12;
        while (qname_end < query.size() && query[qname_end] != 0) {
            qname_end += size_t{1} + query[qname_end];
        }
        qname_end += 1;  // skip the root label
        resp.insert(resp.end(), query.begin() + 12, query.begin() + static_cast<std::ptrdiff_t>(qname_end) + 4);

        // Answer 1: name pointer (0xC0 0x0C), TYPE A, CLASS IN, TTL 60, RDATA.
        resp.push_back(0xC0);
        resp.push_back(0x0C);
        resp.push_back(0x00);
        resp.push_back(0x01);
        resp.push_back(0x00);
        resp.push_back(0x01);
        resp.push_back(0x00);
        resp.push_back(0x00);
        resp.push_back(0x00);
        resp.push_back(0x3C);
        resp.push_back(0x00);
        resp.push_back(0x04);
        resp.push_back(a);
        resp.push_back(b);
        resp.push_back(c);
        resp.push_back(d);

        if (include_unrelated) {
            // Answer 2: fully encoded unrelated owner name + A record.
            auto unrelated = encode_dns_name("unrelated-host.local");
            resp.insert(resp.end(), unrelated.begin(), unrelated.end());
            resp.push_back(0x00);
            resp.push_back(0x01);
            resp.push_back(0x00);
            resp.push_back(0x01);
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x3C);
            resp.push_back(0x00);
            resp.push_back(0x04);
            resp.push_back(1);
            resp.push_back(2);
            resp.push_back(3);
            resp.push_back(4);
        }

        return resp;
    }

    void responder_loop() {
        // Poll with a short timeout so we can check the stop flag.
        while (!stop_flag_.load()) {
            auto wait_res = responder_sock_->wait_for(POLLIN, 100);
            if (!wait_res || *wait_res == 0) {
                continue;
            }

            // Receive the mDNS query.
            std::array<std::uint8_t, 512> recv_buf{};
            SocketAddr src_addr;
            auto n = responder_sock_->recv_from(
                std::span<std::byte>{reinterpret_cast<std::byte*>(recv_buf.data()), recv_buf.size()}, 0, &src_addr);

            if (n <= 0) {
                continue;
            }

            // Only process queries matching our test hostname.
            // This prevents stale multicast packets from other processes
            // (avahi-daemon, systemd-resolved, or previous test runs) from
            // inflating query_count_ and causing flaky failures.
            auto query = std::span<const std::uint8_t>(recv_buf.data(), static_cast<size_t>(n));
            auto encoded = encode_dns_name(test_hostname_);
            if (!qname_matches(query, encoded)) {
                continue;
            }
            query_count_.fetch_add(1);

            // Test hook: with hold_response_ armed, acknowledge the query to
            // the test and park before replying, so the test can cancel a
            // client that is blocked mid-wait deterministically.
            if (hold_response_.load()) {
                if (!query_signalled_.exchange(true)) {
                    query_received_.set_value();
                }
                std::unique_lock lock(hold_mtx_);
                hold_cv_.wait(lock, [&] { return respond_release_ || stop_flag_.load(); });
            }

            // Test hook: precede the genuine reply with a malformed datagram
            // (from the correct source port) — a busy shared multicast group
            // produces these in practice.
            if (send_garbage_first_.load()) {
                std::vector<std::uint8_t> garbage(12, 0x00);  // zeroed header
                garbage.insert(garbage.end(), query.begin(), query.end());
                auto garbage_bytes = std::as_bytes(std::span{garbage});
                [[maybe_unused]] auto gsent = responder_sock_->send_to(garbage_bytes, src_addr);
            }

            // Genuine response from port 5353 (RFC 6762 §6 compliance).
            auto resp = build_response(query, 198, 51, 100, 7, include_unrelated_record_.load());
            auto data = std::as_bytes(std::span{resp});
            [[maybe_unused]] auto sent = responder_sock_->send_to(data, src_addr);
        }
    }

    /// Attacker-style responder: answers matching queries from a NON-5353
    /// source port with a forged IP (1.2.3.4).
    void forger_loop() {
        while (!stop_flag_.load()) {
            auto wait_res = forger_sock_->wait_for(POLLIN, 100);
            if (!wait_res || *wait_res == 0) {
                continue;
            }

            std::array<std::uint8_t, 512> recv_buf{};
            SocketAddr src_addr;
            auto n = forger_sock_->recv_from(
                std::span<std::byte>{reinterpret_cast<std::byte*>(recv_buf.data()), recv_buf.size()}, 0, &src_addr);

            if (n <= 0) {
                continue;
            }

            auto query = std::span<const std::uint8_t>(recv_buf.data(), static_cast<size_t>(n));
            auto encoded = encode_dns_name(test_hostname_);
            if (!qname_matches(query, encoded)) {
                continue;
            }

            // Forged reply from the ephemeral (non-5353) source port.
            auto resp = build_response(query, 1, 2, 3, 4);
            auto data = std::as_bytes(std::span{resp});
            [[maybe_unused]] auto sent = forger_sock_->send_to(data, src_addr);
        }
    }

    std::unique_ptr<Socket> responder_sock_;
    std::thread responder_thread_;
    std::unique_ptr<Socket> forger_sock_;
    std::thread forger_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> query_count_{0};
    ip_mreq mreq_{};
    std::mutex hold_mtx_;
    std::condition_variable hold_cv_;
    bool respond_release_{false};
};

TEST_F(MdnsTest, ResolveMdns_A_Record) {
    MdnsIpSource source(test_hostname_, RecordKind::A, "");
    const auto addrs = source.resolve({});

    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    ASSERT_EQ(addrs->size(), 1U);
    EXPECT_EQ((*addrs)[0].to_string(), "198.51.100.7");
    EXPECT_EQ((*addrs)[0].get_family(), AddressFamily::IPV4);
    EXPECT_EQ(query_count(), 1);
}

TEST_F(MdnsTest, ResolveMdns_ToleratesMalformedDatagram) {
    // A garbage datagram from port 5353 (e.g. another host's malformed
    // packet on the shared multicast group) must be discarded, not abort
    // the query — the genuine reply right behind it still wins.
    send_garbage_first_.store(true);
    MdnsIpSource source(test_hostname_, RecordKind::A, "");
    const auto addrs = source.resolve({});

    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    ASSERT_EQ(addrs->size(), 1U);
    EXPECT_EQ((*addrs)[0].to_string(), "198.51.100.7");
}

// ===========================================================================
// IpSourceFactory — create MdnsIpSource via factory
// ===========================================================================

TEST_F(MdnsTest, Factory_CreateMdnsSource_ResolvesViaMulticast) {
    domain::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::MDNS;
    cfg.ip_source_param = test_hostname_;
    cfg.interface = "";

    const auto source = IpSourceFactory::create(cfg);
    ASSERT_TRUE(source.has_value()) << source.error().message;

    const auto addrs = (*source)->resolve({});
    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    ASSERT_EQ(addrs->size(), 1U);
    EXPECT_EQ((*addrs)[0].to_string(), "198.51.100.7");
    EXPECT_EQ(query_count(), 1);
}

// ===========================================================================
// mDNS — response validation (source port + record ownership)
// ===========================================================================

TEST_F(MdnsTest, ResolveMdns_IgnoresWrongSourcePort) {
    // Regression: a forged reply from a non-5353 source port (RFC 6762 §6
    // violation) must be discarded.  The forger answers the same query with
    // IP 1.2.3.4; the genuine responder (port 5353) answers 198.51.100.7.
    start_forger();

    MdnsIpSource source(test_hostname_, RecordKind::A, "");
    const auto addrs = source.resolve({});

    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    ASSERT_EQ(addrs->size(), 1U);
    EXPECT_EQ((*addrs)[0].to_string(), "198.51.100.7");
    EXPECT_EQ(query_count(), 1);
}

TEST_F(MdnsTest, ResolveMdns_IgnoresUnrelatedRecords) {
    // Regression: answers whose owner name does not match the queried
    // hostname must be filtered out — a response may carry records for
    // other services on the shared multicast group.
    include_unrelated_record_.store(true);

    MdnsIpSource source(test_hostname_, RecordKind::A, "");
    const auto addrs = source.resolve({});

    ASSERT_TRUE(addrs.has_value()) << addrs.error().message;
    ASSERT_EQ(addrs->size(), 1U);
    EXPECT_EQ((*addrs)[0].to_string(), "198.51.100.7");
    EXPECT_EQ(query_count(), 1);
}

// ===========================================================================
// mDNS — cancellation while waiting for a response
// ===========================================================================

// The responder holds its reply after acknowledging the query; triggering the
// token while MdnsIpSource waits must wake the poll and return CANCELLED
// instead of waiting out the 500ms deadline (plan 4.5).
TEST_F(MdnsTest, ResolveMdns_CancelMidWaitReturnsCancelled) {
    hold_response_.store(true);

    MdnsIpSource source(test_hostname_, RecordKind::A, "");
    Utils::CancellationSource cancellation;

    std::optional<IpSourceBase::Result> result;
    std::jthread worker([&] { result = source.resolve(cancellation.token()); });

    // The query reached the responder: resolve() is parked in its wait loop.
    ASSERT_EQ(query_received_.get_future().wait_for(10s), std::future_status::ready)
        << "the mDNS query never reached the responder";

    const auto start = std::chrono::steady_clock::now();
    cancellation.trigger();
    worker.join();
    release_response();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::IpSourceError::Code::CANCELLED);
    // Without cancellation the same lookup would sit in poll() until the
    // 500ms mDNS deadline and return UNAVAILABLE.
    EXPECT_LT(elapsed, 400ms);
}

// ===========================================================================
// mDNS — exception boundary: no ordinary exception crosses resolve()
// ===========================================================================

// A label longer than 63 octets violates the DNS wire format, so the query
// builder throws DnsPacketException before any socket is created. The source
// boundary must classify it as UNAVAILABLE instead of letting it escape.
TEST(MdnsExceptionBoundaryTest, ResolveMdns_MalformedHostnameReturnsUnavailable) {
    const std::string bad_hostname = std::string(64, 'a') + ".local";
    const MdnsIpSource source(bad_hostname, RecordKind::A, "");

    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
}
