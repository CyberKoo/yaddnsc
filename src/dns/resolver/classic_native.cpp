//
// Created by Kotarou on 2026/7/6.
//
// EXPERIMENTAL: self-contained UDP/TCP resolver (no libresolv).
// Not enabled by default — use classic_system for production.
//
#include "classic.h"

#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "uri.h"
#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "network/inet_address.h"
#include "network/socket.h"
#include "exception/socket.h"
#include "exception/dns_lookup.h"
#include "dns/resolver_registry.h"
#include "dns/wire/query.h"

namespace {
    // ── Constants ──
    constexpr int UDP_TIMEOUT_SEC = 1;
    constexpr int TCP_CONNECT_TIMEOUT_SEC = 1;
    constexpr int MAX_DNS_PACKET_SIZE = 4096;

    // ── Build SocketAddr from DNS::Server ──
    struct AddrResult {
        SocketAddr addr;
        int family{AF_UNSPEC};
    };

    [[nodiscard]] AddrResult make_addr(const DnsServer &server) {
        auto parsed = InetAddress::parse(server.address);
        if (!parsed) {
            throw DnsLookupException(
                fmt::format(R"(Invalid DNS server address "{}" — must be an IP address)", server.address),
                DnsError::CONFIG
            );
        }

        auto sa = SocketAddr::from_inet(*parsed, server.port);
        if (!sa) {
            throw DnsLookupException(
                    fmt::format(R"(Failed to build socket address for "{}")", server.address),
                    DnsError::CONFIG
                );
        }

        AddrResult result;
        result.addr = *sa;
        result.family = (parsed->get_family() == AddressFamily::IPV4) ? AF_INET : AF_INET6;
        return result;
    }

    // ── Translate SocketException to DnsLookupException ──
    [[noreturn]] void rethrow_socket(const SocketException &e, const char *context, std::uint64_t resolver_id) {
        // poll() timeout yields ETIMEDOUT (Socket::wait_for / Socket::connect).
        // A signal-interrupted recv could yield EAGAIN.
        // Both should be classified as RETRY so callers can implement back-off logic.
        if (e.get_errno() == ETIMEDOUT || e.get_errno() == EAGAIN) {
            throw DnsLookupException(
                fmt::format(R"(Resolver #{} DNS lookup timed out)", resolver_id),
                DnsError::RETRY
            );
        }
        if (e.get_errno() == ECANCELED) {
            throw DnsLookupException(
                fmt::format(R"(Resolver #{} DNS lookup cancelled)", resolver_id),
                DnsError::CANCELLED
            );
        }
        throw DnsLookupException(
            fmt::format(R"(Resolver #{} DNS lookup: {} {})", resolver_id, context, e.what()),
            DnsError::CONNECTION
        );
    }

    // ── UDP query ──
    [[nodiscard]] std::vector<std::uint8_t> query_udp(const AddrResult &addr, const std::vector<std::uint8_t> &query_packet,
                                        int cancel_fd, std::uint64_t resolver_id) {
        try {
            Socket sock(addr.family, SOCK_DGRAM);

            auto data = std::as_bytes(std::span{query_packet});
            if (auto sent = sock.send_to(data, addr.addr);
                sent != static_cast<ssize_t>(data.size())) {
                int saved_errno = errno;
                throw SocketException(saved_errno, "sendto");
            }

            // Use poll() for receive timeout instead of SO_RCVTIMEO.
            if (sock.wait_for(POLLIN, UDP_TIMEOUT_SEC * 1000, cancel_fd) == 0) {
                throw SocketException(ETIMEDOUT, "UDP query recvfrom");
            }

            std::vector<std::uint8_t> response(MAX_DNS_PACKET_SIZE);
            auto buf = std::as_writable_bytes(std::span{response});
            auto received = sock.recv_from(buf);
            if (received < 0) {
                int saved_errno = errno;
                throw SocketException(saved_errno, "recvfrom");
            }
            response.resize(static_cast<size_t>(received));
            return response;
        } catch (const SocketException &e) {
            rethrow_socket(e, "UDP", resolver_id);
        }
    }

    // ── TCP query (fallback for truncated responses) ──
    [[nodiscard]] std::vector<std::uint8_t> query_tcp(const AddrResult &addr, const std::vector<std::uint8_t> &query_packet,
                                        int cancel_fd, std::uint64_t resolver_id) {
        try {
            Socket sock(addr.family, SOCK_STREAM);

            // Non-blocking connect with poll() timeout (connect already uses poll internally).
            sock.connect(addr.addr, TCP_CONNECT_TIMEOUT_SEC);

            // Send: 2-byte big-endian length prefix + query packet (RFC 1035 §4.2.2).
            const std::uint16_t be_len = htons(static_cast<std::uint16_t>(query_packet.size()));
            std::vector<std::uint8_t> tcp_query(sizeof(be_len));
            std::ranges::copy_n(reinterpret_cast<const std::uint8_t*>(&be_len), sizeof(be_len), tcp_query.begin());
            tcp_query.insert(tcp_query.end(), query_packet.begin(), query_packet.end());

            auto data = std::as_bytes(std::span{tcp_query});
            if (sock.send(data) < 0) {
                int saved_errno = errno;
                throw SocketException(saved_errno, "send");
            }

            // Receive with per-chunk poll() timeout, replacing SO_RCVTIMEO.
            const int timeout_ms = TCP_CONNECT_TIMEOUT_SEC * 1000;
            auto recv_with_timeout = [&sock, cancel_fd](std::span<std::byte> buf) -> ssize_t {
                if (sock.wait_for(POLLIN, timeout_ms, cancel_fd) == 0) {
                    // poll() timed out — no data within the deadline.
                    throw SocketException(ETIMEDOUT, "recv");
                }
                auto n = sock.recv(buf);
                if (n == 0) {
                    // Peer closed the connection.
                    throw SocketException(ECONNRESET, "recv");
                }
                return n;   // n < 0 carries errno to the caller
            };

            // Receive: 2-byte big-endian length prefix.
            std::uint16_t be_rsp_len = 0;
            auto len_buf = std::as_writable_bytes(std::span{&be_rsp_len, 1});
            {
                size_t total = 0;
                while (total < len_buf.size()) {
                    auto n = recv_with_timeout(len_buf.subspan(total));
                    if (n < 0) {
                        int saved_errno = errno;
                        throw SocketException(saved_errno, "recv");
                    }
                    total += static_cast<size_t>(n);
                }
            }

            const size_t rsp_len = ntohs(be_rsp_len);
            if (rsp_len == 0 || rsp_len > MAX_DNS_PACKET_SIZE) {
                throw DnsLookupException(
                    fmt::format("Invalid DNS response length: {}", rsp_len),
                    DnsError::PARSE
                );
            }

            // Receive response body.
            std::vector<std::uint8_t> response(rsp_len);
            auto rsp_buf = std::as_writable_bytes(std::span{response});
            {
                size_t total = 0;
                while (total < rsp_buf.size()) {
                    auto n = recv_with_timeout(rsp_buf.subspan(total));
                    if (n < 0) {
                        int saved_errno = errno;
                        throw SocketException(saved_errno, "recv");
                    }
                    total += static_cast<size_t>(n);
                }
            }
            return response;
        } catch (const SocketException &e) {
            rethrow_socket(e, "TCP", resolver_id);
        }
    }

    // ── Check TC (Truncation) bit in DNS header ──
    [[nodiscard]] bool is_truncated(const std::vector<std::uint8_t> &response) {
        // TC is bit 2 of the second byte in the flags field (byte 2 of the header, 0-indexed).
        return response.size() >= 3 && (response[2] & 0x02) != 0;
    }


} // anonymous namespace

// ===========================================================================
//  ClassicResolver::Impl  —  private implementation
// ===========================================================================

struct ClassicResolver::Impl {
    explicit Impl(DnsServer server, std::uint64_t id);

    ~Impl() = default;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsLookupException>
    query(const std::string &host_str, RecordKind type, int cancel_fd = -1) const;

    std::uint64_t id_;
    DnsServer server_;
    Uri uri_;
    AddrResult addr_;
};

ClassicResolver::Impl::Impl(DnsServer server, std::uint64_t id)
    : id_(id), server_(std::move(server)), uri_(Uri::parse(server_.address)), addr_(make_addr(server_)) {
}

std::expected<std::vector<std::uint8_t>, DnsLookupException> ClassicResolver::Impl::query(const std::string &host_str, RecordKind type,
                                                                                              int cancel_fd) const {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

    const auto record_type = DNS::Util::type_to_record_type(type);
    SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id_, host_str, static_cast<std::uint16_t>(record_type),
                 uri_.get_host_literal(), server_.port);

    // Build query packet using the native wire-format builder.
    auto query_packet = DNS::mkquery_native(host_str, record_type);

    // Try UDP first.
    auto response = query_udp(addr_, query_packet, cancel_fd, id_);
    DNS::Validator::validate_response(query_packet, response);

    // Fall back to TCP if response is truncated.
    if (is_truncated(response)) {
        SPDLOG_TRACE(R"(Resolver #{} UDP response truncated for "{}", falling back to TCP)", id_, host_str);
        response = query_tcp(addr_, query_packet, cancel_fd, id_);
        DNS::Validator::validate_response(query_packet, response);
    }

    return response;
    } catch (const DnsLookupException &e) {
        return std::unexpected(e);
    }
}

ClassicResolver::ClassicResolver(DnsServer server)
    : impl_(std::make_unique<Impl>(std::move(server), get_id())) {
}

ClassicResolver::~ClassicResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsLookupException> ClassicResolver::query(const std::string &host, RecordKind type,
                                                                                       int cancel_fd) const noexcept {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
[[maybe_unused]] DnsResolverRegistry::Registrar _classic(
    "",
    [](const DnsServer &server) -> std::shared_ptr<ResolverBase> {
        return std::make_shared<ClassicResolver>(server);
    }
);
}
