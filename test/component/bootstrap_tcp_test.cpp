//
// Bootstrap TCP fallback against an in-process DNS server.
//
// UDP answers with a validated, truncated header. The address record exists
// only on the TCP response, so resolve_bootstrap has to open TCP to the same
// IP and port.
//

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "domain/config/dns_config.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "support/util/cancellation_token.hpp"

using namespace std::chrono_literals;

namespace {

constexpr std::uint8_t ANSWER_A[] = {
    0xC0, 0x0C,              // name → question at offset 12
    0x00, 0x01,              // A
    0x00, 0x01,              // IN
    0x00, 0x00, 0x00, 0x3C,  // TTL 60
    0x00, 0x04,              // RDLENGTH
    198,  51,   100,  99,
};

[[nodiscard]] std::optional<std::size_t> question_end(const std::span<const std::uint8_t> packet) {
    if (packet.size() < 12) {
        return std::nullopt;
    }
    std::size_t offset = 12;
    while (offset < packet.size()) {
        const auto label = packet[offset];
        if (label == 0) {
            offset += 1;
            break;
        }
        if ((label & 0xC0U) != 0) {
            if (offset + 1 >= packet.size()) {
                return std::nullopt;
            }
            offset += 2;
            break;
        }
        if (offset + 1U + label > packet.size()) {
            return std::nullopt;
        }
        offset += 1U + label;
    }
    if (offset + 4 > packet.size()) {
        return std::nullopt;
    }
    return offset + 4;
}

[[nodiscard]] std::vector<std::uint8_t> dns_response(const std::span<const std::uint8_t> query, const bool truncated) {
    const auto end = question_end(query);
    if (!end) {
        return {};
    }
    std::vector<std::uint8_t> response(query.begin(), query.begin() + static_cast<std::ptrdiff_t>(*end));
    const auto rd = static_cast<std::uint8_t>(query[2] & 0x01U);
    response[2] = static_cast<std::uint8_t>(0x80U | rd | (truncated ? 0x02U : 0U));
    response[3] = 0x80;  // RA
    response[4] = 0x00;
    response[5] = 0x01;  // QDCOUNT
    response[6] = 0x00;
    response[7] = truncated ? 0x00 : 0x01;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;
    if (!truncated) {
        response.insert(response.end(), std::begin(ANSWER_A), std::end(ANSWER_A));
    }
    return response;
}

[[nodiscard]] bool read_full(Socket& sock, const std::span<std::uint8_t> buf) {
    std::size_t filled = 0;
    while (filled < buf.size()) {
        auto n = sock.recv_some(std::as_writable_bytes(buf.subspan(filled)));
        if (!n || *n == 0) {
            return false;
        }
        filled += *n;
    }
    return true;
}

[[nodiscard]] bool write_full(Socket& sock, const std::span<const std::uint8_t> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = sock.send_some(std::as_bytes(data.subspan(sent)));
        if (!n || *n == 0) {
            return false;
        }
        sent += *n;
    }
    return true;
}

/// UDP and TCP on one loopback port. UDP is truncated; TCP carries the A record.
class TruncatingDnsServer {
public:
    TruncatingDnsServer() {
        auto ip = InetAddress::parse("127.0.0.1");
        if (!ip) {
            error_ = "loopback address";
            return;
        }
        auto wildcard = SocketAddr::from_inet(*ip, 0);
        if (!wildcard) {
            error_ = "socket address";
            return;
        }

        auto tcp = Socket::open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        auto udp = Socket::open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!tcp || !udp) {
            error_ = "socket";
            return;
        }
        if (!tcp->set_reuseaddr(true) || !udp->set_reuseaddr(true) || !tcp->bind(*wildcard)) {
            error_ = "bind";
            return;
        }
        auto bound = tcp->get_sockname();
        if (!bound || !udp->bind(*bound) || !tcp->listen(4)) {
            error_ = "listen";
            return;
        }
        port_ = bound->port();
        tcp_ = std::move(*tcp);
        udp_ = std::move(*udp);
        thread_ = std::thread([this] { serve(); });
    }

    ~TruncatingDnsServer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    TruncatingDnsServer(const TruncatingDnsServer&) = delete;
    TruncatingDnsServer& operator=(const TruncatingDnsServer&) = delete;

    [[nodiscard]] bool ok() const noexcept { return port_ != 0 && error_.empty(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] int tcp_sessions() const noexcept { return tcp_sessions_.load(std::memory_order_acquire); }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd pfds[2]{};
            pfds[0] = {.fd = udp_.native_handle(), .events = POLLIN, .revents = 0};
            pfds[1] = {.fd = tcp_.native_handle(), .events = POLLIN, .revents = 0};
            const int ready = ::poll(pfds, 2, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error_ = "poll";
                return;
            }
            if (ready == 0) {
                continue;
            }
            if ((pfds[0].revents & POLLIN) != 0) {
                reply_udp();
            }
            if ((pfds[1].revents & POLLIN) != 0) {
                reply_tcp();
            }
        }
    }

    void reply_udp() {
        std::array<std::uint8_t, 512> query{};
        SocketAddr src;
        auto n = udp_.recv_from(std::as_writable_bytes(std::span{query}), &src);
        if (!n || *n < 12) {
            return;
        }
        const auto response = dns_response(std::span{query}.first(*n), true);
        if (response.empty()) {
            return;
        }
        (void) udp_.send_to(std::as_bytes(std::span{response}), src);
    }

    void reply_tcp() {
        auto accepted = tcp_.accept();
        if (!accepted) {
            return;
        }
        tcp_sessions_.fetch_add(1, std::memory_order_release);
        timeval budget{.tv_sec = 1, .tv_usec = 0};
        (void) accepted->set_option(SOL_SOCKET, SO_RCVTIMEO, budget);

        std::array<std::uint8_t, 2> len_buf{};
        if (!read_full(*accepted, len_buf)) {
            return;
        }
        std::uint16_t be_len = 0;
        std::memcpy(&be_len, len_buf.data(), sizeof(be_len));
        const std::size_t len = ntohs(be_len);
        if (len == 0 || len > 4096) {
            return;
        }
        std::vector<std::uint8_t> query(len);
        if (!read_full(*accepted, query)) {
            return;
        }
        const auto response = dns_response(query, false);
        if (response.empty() || response.size() > 65535) {
            return;
        }
        const auto be_rsp = htons(static_cast<std::uint16_t>(response.size()));
        std::vector<std::uint8_t> framed(sizeof(be_rsp) + response.size());
        std::memcpy(framed.data(), &be_rsp, sizeof(be_rsp));
        std::memcpy(framed.data() + sizeof(be_rsp), response.data(), response.size());
        (void) write_full(*accepted, framed);
    }

    Socket tcp_;
    Socket udp_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> tcp_sessions_{0};
    std::uint16_t port_ = 0;
    std::string error_;
};

}  // namespace

TEST(BootstrapTcp, TruncatedUdp_RetriesOverTcp) {
    TruncatingDnsServer server;
    ASSERT_TRUE(server.ok()) << server.error();

    const std::vector<Config::DnsServer> servers{{"127.0.0.1", server.port()}};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    const auto result = DNS::resolve_bootstrap("truncate.yaddnsc.test", AddressFamily::IPV4, servers, deadline, {});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ((*result)[0].to_string(), "198.51.100.99");
    EXPECT_EQ(server.tcp_sessions(), 1);
}
