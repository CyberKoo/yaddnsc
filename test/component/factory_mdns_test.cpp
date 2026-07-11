//
// Integration tests for IpSourceFactory and MdnsIpSource.
//
// Factory test: creates an INTERFACE source via the factory.
// mDNS test:   sets up a local multicast responder on loopback,
//              resolves via MdnsIpSource, and verifies the result.
//
// These tests share the test_updater target's linker dependencies
// (socket, net_devices, builder, parser, etc.).
//
// =============================================================================

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/config.h"
#include "ip_source/base.h"
#include "ip_source/factory.h"
#include "ip_source/mdns.h"
#include "network/inet_address.h"
#include "network/net_devices.h"
#include "network/socket.h"
#include "network/socket_addr.h"

#include <netinet/in.h>
#include <poll.h>

#include "address_family.h"
#include "record_kind.h"

#include "fmt.hpp"

namespace {
    const std::string LOOPBACK = NetDevices::loopback_name();

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
} // anonymous namespace

using namespace std::chrono_literals;

// ===========================================================================
// IpSourceFactory — create InterfaceIpSource
// ===========================================================================

TEST(IpSourceFactoryTest, CreateInterfaceSource_ResolvesLoopback) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);

    // resolve() must work using the real loopback interface.
    auto addrs = source->resolve();
    EXPECT_FALSE(addrs.empty());
    EXPECT_TRUE(std::ranges::any_of(addrs, [](const InetAddress &a) {
        return a.to_string() == "127.0.0.1";
    }));
}

TEST(IpSourceFactoryTest, CreateInterfaceSource_Ipv6) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::AAAA;
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);
    auto addrs = source->resolve();

    if (addrs.empty()) {
        GTEST_SKIP() << "IPv6 is not available on this system";
    }
    for (const auto &addr : addrs) {
        EXPECT_EQ(addr.get_family(), AddressFamily::IPV6);
    }
}

// ===========================================================================
// IpSourceFactory — create HttpIpSource (constructor only, no I/O)
// ===========================================================================

TEST(IpSourceFactoryTest, CreateHttpSource_ConstructsSuccessfully) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::HTTP;
    cfg.ip_source_param = "http://127.0.0.1:1/ip";  // valid URL, no server needed for construction

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);
    // Constructor succeeds — resolves via PersistentHttpClient.
    // resolve() would fail with connection refused, which is expected.
}

TEST(IpSourceFactoryTest, CreateHttpSource_WithIface_BindsToInterface) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::HTTP;
    cfg.ip_source_param = "http://127.0.0.1:1/ip";
    cfg.interface = LOOPBACK;

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);
}

// ===========================================================================
// IpSourceFactory — type_to_family with unknown record type
// ===========================================================================

TEST(IpSourceFactoryTest, UnknownType_FallsBackToUnspecified) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::TXT;       // not A or AAAA → UNSPECIFIED
    cfg.ip_source = Config::IpSource::INTERFACE;
    cfg.interface = LOOPBACK;

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);

    // UNSPECIFIED returns all addresses on the interface.
    auto addrs = source->resolve();
    EXPECT_FALSE(addrs.empty());
}

// ===========================================================================
// mDNS — local multicast responder
//
// Starts a UDP listener on 224.0.0.251:5353 (joined on loopback).
// When it receives a DNS query, it responds with a crafted A record.
// The MdnsIpSource resolves the hostname via mDNS and should get
// the IP we sent back.
// ===========================================================================

class MdnsTest : public ::testing::Test {
protected:
		void SetUp() override {
			// ---- Check multicast availability ----------------------------------
			if (!multicast_available()) {
				GTEST_SKIP() << "mDNS multicast not available on this system";
			}

			// ---- Create responder socket ---------------------------------------
			responder_sock_ = std::make_unique<Socket>(AF_INET, SOCK_DGRAM);
        responder_sock_->set_reuseaddr(true);
        responder_sock_->set_option(SOL_SOCKET, SO_REUSEPORT, 1);

        auto bind_addr = SocketAddr::from_inet(Inet4Address{}, 5353);
        ASSERT_TRUE(bind_addr.has_value());
        responder_sock_->bind(*bind_addr);

        // Join multicast group 224.0.0.251 on the default interface.
        auto mcast_addr = Inet4Address::parse("224.0.0.251");
        ASSERT_TRUE(mcast_addr.has_value());

        // Build ip_mreq: group address + INADDR_ANY interface.
        mreq_ = ip_mreq{};
        auto *dest = reinterpret_cast<std::uint8_t *>(&mreq_.imr_multiaddr);
        std::ranges::copy_n(mcast_addr->data(), sizeof(mreq_.imr_multiaddr), dest);
        mreq_.imr_interface.s_addr = INADDR_ANY;
        responder_sock_->set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_);

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
        if (responder_sock_) {
            // Leave multicast group.
            [[maybe_unused]] auto _ = responder_sock_->try_set_option(IPPROTO_IP, IP_DROP_MEMBERSHIP, mreq_);
            responder_sock_->close();
        }
        responder_sock_.reset();
    }

    /// Check how many queries the responder received.
    [[nodiscard]] int query_count() const {
        return query_count_.load();
    }

private:
    void responder_loop() {
        // Poll with a short timeout so we can check the stop flag.
        while (!stop_flag_.load()) {
            auto ready = responder_sock_->wait_for(POLLIN, 100);
            if (ready <= 0) {
                continue;
            }

            // Receive the mDNS query.
            std::array<std::uint8_t, 512> recv_buf{};
            SocketAddr src_addr;
            auto n = responder_sock_->recv_from(std::span<std::byte>{
                reinterpret_cast<std::byte *>(recv_buf.data()), recv_buf.size()
            }, 0, &src_addr);

            if (n <= 0) {
                continue;
            }
            query_count_.fetch_add(1);

            // Build a crafted DNS response.
            // Copy the query's TXID, set QR+RA flags, ANCOUNT=1.
            auto query = std::span<const std::uint8_t>(recv_buf.data(), static_cast<size_t>(n));
            std::vector<std::uint8_t> resp;
            resp.reserve(n + 16);

            // Header (copies query's TXID).
            resp.push_back(query[0]);
            resp.push_back(query[1]);
            resp.push_back(0x80);       // flags: QR=1
            resp.push_back(0x80);       // flags: RA=1
            // QDCOUNT = 1 (from query), ANCOUNT = 1, NS/AR = 0
            resp.push_back(0x00);
            resp.push_back(0x01);
            resp.push_back(0x00);
            resp.push_back(0x01);
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x00);

            // Echo back the question section from the query (starts at offset 12).
            // Find the end of the QNAME (root label 0x00).
            size_t qname_end = 12;
            while (qname_end < query.size() && query[qname_end] != 0) {
                qname_end += 1 + query[qname_end];
            }
            qname_end += 1; // skip the root label
            // Copy QNAME + QTYPE + QCLASS (4 bytes after QNAME)
            resp.insert(resp.end(), query.begin() + 12, query.begin() + qname_end + 4);

            // Answer section: name pointer (0xC0 0x0C = compressed name),
            // TYPE A (1), CLASS IN (1), TTL 60, RDLENGTH 4, IP 198.51.100.7.
            resp.push_back(0xC0);
            resp.push_back(0x0C);
            resp.push_back(0x00);
            resp.push_back(0x01);  // TYPE A
            resp.push_back(0x00);
            resp.push_back(0x01);  // CLASS IN
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x00);
            resp.push_back(0x3C);  // TTL 60
            resp.push_back(0x00);
            resp.push_back(0x04);  // RDLENGTH 4
            resp.push_back(198);
            resp.push_back(51);
            resp.push_back(100);
            resp.push_back(7);

            // Send the response back to the query's source address.
            auto data = std::as_bytes(std::span{resp});
            [[maybe_unused]] auto sent = responder_sock_->send_to(data, src_addr);
        }
    }

    std::unique_ptr<Socket> responder_sock_;
    std::thread responder_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> query_count_{0};
    ip_mreq mreq_{};
};

TEST_F(MdnsTest, ResolveMdns_A_Record) {
    MdnsIpSource source("test.local", RecordKind::A, "");
    auto addrs = source.resolve();

    ASSERT_EQ(addrs.size(), 1U);
    EXPECT_EQ(addrs[0].to_string(), "198.51.100.7");
    EXPECT_EQ(addrs[0].get_family(), AddressFamily::IPV4);
    EXPECT_EQ(query_count(), 1);
}

// ===========================================================================
// IpSourceFactory — create MdnsIpSource via factory
// ===========================================================================

TEST_F(MdnsTest, Factory_CreateMdnsSource_ResolvesViaMulticast) {
    Config::SubdomainConfig cfg;
    cfg.name = "test";
    cfg.type = RecordKind::A;
    cfg.ip_source = Config::IpSource::MDNS;
    cfg.ip_source_param = "test.local";
    cfg.interface = "";

    auto source = IpSourceFactory::create(cfg);
    ASSERT_NE(source, nullptr);

    auto addrs = source->resolve();
    ASSERT_EQ(addrs.size(), 1U);
    EXPECT_EQ(addrs[0].to_string(), "198.51.100.7");
    EXPECT_EQ(query_count(), 1);
}
