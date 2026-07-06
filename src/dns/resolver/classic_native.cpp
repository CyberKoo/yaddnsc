//
// Created by Kotarou on 2026/7/6.
//
#include "classic.h"

#include <arpa/inet.h>

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "dns/proto/mkquery.h"
#include "network/inet_address.h"
#include "network/socket.h"
#include "exception/socket.h"
#include "exception/dns_lookup.h"

namespace {
    // ── Constants ──
    constexpr int UDP_TIMEOUT_SEC = 5;
    constexpr int TCP_CONNECT_TIMEOUT_SEC = 5;
    constexpr int MAX_DNS_PACKET_SIZE = 4096;

    // ── Build SocketAddr from DNS::Server ──
    struct AddrResult {
        SocketAddr addr;
        int family{AF_UNSPEC};
    };

    AddrResult make_addr(const DNS::Server &server) {
        auto parsed = InetAddress::parse(server.address);
        if (!parsed) {
            throw DnsLookupException(
                fmt::format(R"(Invalid DNS server address "{}" — must be an IP address)", server.address),
                DNS::Error::PARSE
            );
        }

        auto sa = SocketAddr::from_inet(*parsed, server.port);
        if (!sa) {
            throw DnsLookupException(
                fmt::format(R"(Failed to build socket address for "{}")", server.address),
                DNS::Error::PARSE
            );
        }

        AddrResult result;
        result.addr = *sa;
        result.family = (parsed->get_family() == AddressFamily::IPV4) ? AF_INET : AF_INET6;
        return result;
    }

    // ── Translate SocketException to DnsLookupException ──
    [[noreturn]] void rethrow_socket(const SocketException &e, const char *context) {
        // SO_RCVTIMEO expiry yields EAGAIN (EWOULDBLOCK); connect() timeout yields ETIMEDOUT.
        // Both should be classified as RETRY so callers can implement back-off logic.
        if (e.get_errno() == ETIMEDOUT || e.get_errno() == EAGAIN) {
            throw DnsLookupException(
                fmt::format("{} timed out", context),
                DNS::Error::RETRY
            );
        }
        throw DnsLookupException(
            std::string(context) + ": " + e.what(),
            DNS::Error::CONNECTION
        );
    }

    // ── UDP query ──
    std::vector<std::uint8_t> query_udp(const AddrResult &addr, const std::vector<std::uint8_t> &query_packet) {
        try {
            Socket sock(addr.family, SOCK_DGRAM);
            sock.set_option(SOL_SOCKET, SO_RCVTIMEO, timeval{UDP_TIMEOUT_SEC, 0});

            auto data = std::as_bytes(std::span{query_packet});
            if (sock.send_to(data, addr.addr) != static_cast<ssize_t>(data.size())) {
                throw SocketException(errno, "sendto");
            }

            std::vector<std::uint8_t> response(MAX_DNS_PACKET_SIZE);
            auto buf = std::as_writable_bytes(std::span{response});
            auto received = sock.recv_from(buf);
            if (received < 0) {
                throw SocketException(errno, "recvfrom");
            }
            response.resize(static_cast<size_t>(received));
            return response;
        } catch (const SocketException &e) {
            rethrow_socket(e, "UDP query");
        }
    }

    // ── TCP query (fallback for truncated responses) ──
    std::vector<std::uint8_t> query_tcp(const AddrResult &addr, const std::vector<std::uint8_t> &query_packet) {
        try {
            Socket sock(addr.family, SOCK_STREAM);

            // Non-blocking connect with poll timeout.
            sock.connect(addr.addr, TCP_CONNECT_TIMEOUT_SEC);

            sock.set_option(SOL_SOCKET, SO_RCVTIMEO, timeval{TCP_CONNECT_TIMEOUT_SEC, 0});

            // Send: 2-byte big-endian length prefix + query packet (RFC 1035 §4.2.2).
            const std::uint16_t be_len = htons(static_cast<std::uint16_t>(query_packet.size()));
            std::vector<std::uint8_t> tcp_query(sizeof(be_len));
            std::memcpy(tcp_query.data(), &be_len, sizeof(be_len));
            tcp_query.insert(tcp_query.end(), query_packet.begin(), query_packet.end());

            auto data = std::as_bytes(std::span{tcp_query});
            if (sock.send(data) < 0) {
                throw SocketException(errno, "send");
            }

            // Receive: 2-byte big-endian length prefix.
            std::uint16_t be_rsp_len = 0;
            auto len_buf = std::as_writable_bytes(std::span{&be_rsp_len, 1});
            if (sock.recv_exact(len_buf) < 0) {
                throw SocketException(errno, "recv");
            }

            const size_t rsp_len = ntohs(be_rsp_len);
            if (rsp_len == 0 || rsp_len > MAX_DNS_PACKET_SIZE) {
                throw DnsLookupException(
                    fmt::format("Invalid DNS response length: {}", rsp_len),
                    DNS::Error::PARSE
                );
            }

            // Receive response body.
            std::vector<std::uint8_t> response(rsp_len);
            auto rsp_buf = std::as_writable_bytes(std::span{response});
            if (sock.recv_exact(rsp_buf) < 0) {
                throw SocketException(errno, "recv");
            }
            return response;
        } catch (const SocketException &e) {
            rethrow_socket(e, "TCP query");
        }
    }

    // ── Check TC (Truncation) bit in DNS header ──
    bool is_truncated(const std::vector<std::uint8_t> &response) {
        // TC is bit 2 of the second byte in the flags field (byte 2 of the header, 0-indexed).
        return response.size() >= 3 && (response[2] & 0x02) != 0;
    }

    // ── Validate DNS response against the original query ──
    void validate_response(const std::vector<std::uint8_t> &response,
                           const std::vector<std::uint8_t> &query) {
        // Minimum DNS header size per RFC 1035 §4.1.1.
        if (response.size() < 12) {
            throw DnsLookupException(
                fmt::format("DNS response too short: {} bytes (minimum 12)", response.size()),
                DNS::Error::PARSE
            );
        }

        // QR bit (bit 7 of byte 2) must be set for a response.
        if ((response[2] & 0x80) == 0) {
            throw DnsLookupException(
                "DNS response has QR=0 (not a response)",
                DNS::Error::PARSE
            );
        }

        // Transaction ID must match the one in the outgoing query.
        if (response[0] != query[0] || response[1] != query[1]) {
            throw DnsLookupException(
                "DNS response transaction ID mismatch",
                DNS::Error::PARSE
            );
        }
    }
} // anonymous namespace

// ===========================================================================
//  ClassicResolver::Impl  —  private implementation
// ===========================================================================

struct ClassicResolver::Impl {
    explicit Impl(DNS::Server server, std::uint64_t id);

    ~Impl() = default;

    [[nodiscard]] std::vector<std::uint8_t> query(const std::string &host_str, DNS::Type type) const;

    std::uint64_t id_;
    DNS::Server server_;
    AddrResult addr_;
};

ClassicResolver::Impl::Impl(DNS::Server server, std::uint64_t id)
    : id_(id), server_(std::move(server)), addr_(make_addr(server_)) {
}

std::vector<std::uint8_t> ClassicResolver::Impl::query(const std::string &host_str, DNS::Type type) const {
    SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

    const auto ns_type = DNS::to_ns_type(type);
    SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id_, host_str, ns_type,
                 server_.address, server_.port);

    // Build query packet using mkquery_manual.
    auto query_packet = DNS::mkquery_manual(host_str, ns_type);

    // Try UDP first.
    auto response = query_udp(addr_, query_packet);
    validate_response(response, query_packet);

    // Fall back to TCP if response is truncated.
    if (is_truncated(response)) {
        SPDLOG_TRACE(R"(Resolver #{} UDP response truncated for "{}", falling back to TCP)", id_, host_str);
        response = query_tcp(addr_, query_packet);
        validate_response(response, query_packet);
    }

    return response;
}

// ===========================================================================
//  ClassicResolver  —  public API
// ===========================================================================

ClassicResolver::ClassicResolver(DNS::Server server)
    : impl_(std::make_unique<Impl>(std::move(server), get_id())) {
}

ClassicResolver::~ClassicResolver() = default;

std::vector<std::uint8_t> ClassicResolver::query(const std::string &host, DNS::Type type) const {
    return impl_->query(host, type);
}
