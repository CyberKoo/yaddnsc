//
// Created by Kotarou on 2026/7/6.
//
// EXPERIMENTAL: self-contained UDP/TCP resolver (no libresolv).
// Not enabled by default — use classic_system for production.
//
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <expected>

#include "dns/resolver_registry.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/query.h"
#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "exception/dns_packet.h"
#include "exception/socket.h"
#include "network/inet_address.h"
#include "network/socket.h"

#include "classic.h"
#include "dns_error.h"
#include "uri.h"

#include "fmt.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

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

    [[nodiscard]] AddrResult make_addr(const Config::DnsServer &server) {
        auto parsed = InetAddress::parse(server.address);
        if (!parsed) {
            throw DnsLookupException(
                fmt::format(R"(Invalid DNS server address "{}" — must be an IP address)", server.address),
                DnsError::CONFIG
            );
        }

        auto sa = SocketAddr::from_inet(*parsed, server.port);
        if (!sa) {
            throw DnsLookupException(fmt::format(R"(Failed to build socket address for "{}")", server.address),
                                     DnsError::CONFIG
            );
        }

        AddrResult result;
        result.addr = *sa;
        result.family = parsed->get_family() == AddressFamily::IPV4 ? AF_INET : AF_INET6;
        return result;
    }

    // ── Helpers to translate Socket error conditions ──

    /// Translate wait_for / connect timeout into DnsError.
    [[nodiscard]] DnsError classify_socket_error(int errnum) {
        if (errnum == ETIMEDOUT || errnum == EAGAIN)
            return DnsError::RETRY;
        if (errnum == ECANCELED)
            return DnsError::CANCELLED;
        return DnsError::CONNECTION;
    }

    [[nodiscard]] std::string socket_error_msg(std::uint64_t resolver_id, const char *context, int errnum) {
        return fmt::format(R"(Resolver #{} DNS lookup: {} {})", resolver_id, context, std::strerror(errnum));
    }

    // ── UDP query ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query_udp(
        const AddrResult &addr, std::span<const uint8_t> query_packet, int cancel_fd, std::uint64_t resolver_id) {
        // Socket constructor may throw SocketException on OS resource
        // exhaustion — let it propagate.
        Socket sock(addr.family, SOCK_DGRAM);

        auto data = std::as_bytes(std::span{query_packet});
        if (auto sent = sock.send_to(data, addr.addr); sent != static_cast<ssize_t>(data.size())) {
            return std::unexpected(DnsErrorInfo{
                DnsError::CONNECTION,
                fmt::format(R"(Resolver #{} UDP sendto failed: {})", resolver_id, std::strerror(errno))
            });
        }

        // wait_for may throw SocketException on timeout/cancellation —
        // catch at this I/O boundary and translate to expected.
        try {
            if (sock.wait_for(POLLIN, UDP_TIMEOUT_SEC * 1000, cancel_fd) == 0) {
                return std::unexpected(DnsErrorInfo{
                    DnsError::RETRY,
                    fmt::format(R"(Resolver #{} UDP query timed out)", resolver_id)
                });
            }
        } catch (const SocketException &e) {
            const auto ec = classify_socket_error(e.get_errno());
            return std::unexpected(DnsErrorInfo{ec, socket_error_msg(resolver_id, "UDP wait_for", e.get_errno())});
        }

        std::vector<std::uint8_t> response(MAX_DNS_PACKET_SIZE);
        auto buf = std::as_writable_bytes(std::span{response});
        auto received = sock.recv_from(buf);
        if (received < 0) {
            return std::unexpected(DnsErrorInfo{
                DnsError::CONNECTION,
                fmt::format(R"(Resolver #{} UDP recvfrom failed: {})", resolver_id, std::strerror(errno))
            });
        }
        response.resize(static_cast<size_t>(received));
        return response;
    }

    // ── TCP query (fallback for truncated responses) ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query_tcp(
        const AddrResult &addr, std::span<const uint8_t> query_packet, int cancel_fd, std::uint64_t resolver_id) {
        // Socket constructor may throw SocketException on OS resource
        // exhaustion — let it propagate.
        Socket sock(addr.family, SOCK_STREAM);

        // connect returns expected<void, ConnectError> — handle inline.
        auto conn = sock.connect(addr.addr, TCP_CONNECT_TIMEOUT_SEC);
        if (!conn) {
            DnsError ec;
            switch (conn.error()) {
                case ConnectError::TimedOut:
                    ec = DnsError::RETRY;
                    break;
                case ConnectError::Cancelled:
                    ec = DnsError::CANCELLED;
                    break;
                default:
                    ec = DnsError::CONNECTION;
                    break;
            }
            return std::unexpected(DnsErrorInfo{ec, fmt::format(R"(Resolver #{} TCP connect failed)", resolver_id)});
        }

        // Send: 2-byte big-endian length prefix + query packet (RFC 1035 §4.2.2).
        const std::uint16_t be_len = htons(static_cast<std::uint16_t>(query_packet.size()));
        std::vector<std::uint8_t> tcp_query(sizeof(be_len));
        std::ranges::copy_n(reinterpret_cast<const std::uint8_t *>(&be_len), sizeof(be_len), tcp_query.begin());
        tcp_query.insert(tcp_query.end(), query_packet.begin(), query_packet.end());

        auto data = std::as_bytes(std::span{tcp_query});
        if (sock.send(data) < 0) {
            return std::unexpected(DnsErrorInfo{
                DnsError::CONNECTION,
                fmt::format(R"(Resolver #{} TCP send failed: {})", resolver_id, std::strerror(errno))
            });
        }

        // Receive helper: wait with poll, then recv.
        // May throw SocketException on timeout/cancellation — caught below.
        // Returns bytes read on success, or error on failure.
        auto recv_with_timeout = [&sock, cancel_fd, resolver_id](std::span<std::byte> buf)
            -> std::expected<size_t, DnsErrorInfo> {
            try {
                if (sock.wait_for(POLLIN, TCP_CONNECT_TIMEOUT_SEC * 1000, cancel_fd) == 0) {
                    return std::unexpected(DnsErrorInfo{
                        DnsError::RETRY,
                        fmt::format(R"(Resolver #{} TCP recv timed out)", resolver_id)
                    });
                }
            } catch (const SocketException &e) {
                const auto ec = classify_socket_error(e.get_errno());
                return std::unexpected(DnsErrorInfo{ec, socket_error_msg(resolver_id, "TCP wait_for", e.get_errno())});
            }

            auto n = sock.recv(buf);
            if (n == 0) {
                return std::unexpected(DnsErrorInfo{
                    DnsError::CONNECTION,
                    fmt::format(R"(Resolver #{} TCP connection closed by peer)", resolver_id)
                });
            }
            if (n < 0) {
                return std::unexpected(DnsErrorInfo{
                    DnsError::CONNECTION,
                    fmt::format(R"(Resolver #{} TCP recv failed: {})", resolver_id, std::strerror(errno))
                });
            }
            return static_cast<size_t>(n);
        };

        // Receive: 2-byte big-endian length prefix.
        std::uint16_t be_rsp_len = 0;
        auto len_buf = std::as_writable_bytes(std::span{&be_rsp_len, 1});
        {
            size_t total = 0;
            while (total < len_buf.size()) {
                auto n = recv_with_timeout(len_buf.subspan(total));
                if (!n) {
                    return std::unexpected(std::move(n.error()));
                }
                total += *n;
            }
        }

        const size_t rsp_len = ntohs(be_rsp_len);
        if (rsp_len == 0 || rsp_len > MAX_DNS_PACKET_SIZE) {
            return std::unexpected(DnsErrorInfo{
                DnsError::PARSE,
                fmt::format("Invalid DNS response length: {}", rsp_len)
            });
        }

        // Receive response body.
        std::vector<std::uint8_t> response(rsp_len);
        auto rsp_buf = std::as_writable_bytes(std::span{response});
        {
            size_t total = 0;
            while (total < rsp_buf.size()) {
                auto n = recv_with_timeout(rsp_buf.subspan(total));
                if (!n) {
                    return std::unexpected(std::move(n.error()));
                }
                total += *n;
            }
        }
        return response;
    }

    // ── Check TC (Truncation) bit in DNS header ──
    [[nodiscard]] bool is_truncated(const std::vector<std::uint8_t> &response) {
        // TC is bit 2 of the second byte in the flags field (byte 2 of the header, 0-indexed).
        return response.size() >= DNS::HEADER_SIZE && (response[2] & 0x02) != 0;
    }
} // anonymous namespace

// ===========================================================================
//  ClassicResolver::Impl  —  private implementation
// ===========================================================================

struct ClassicResolver::Impl {
    explicit Impl(Config::DnsServer server, std::uint64_t id);

    ~Impl() = default;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host_str, RecordKind type, int cancel_fd = -1) const;

    std::uint64_t id_;
    Config::DnsServer server_;
    Uri uri_;
    AddrResult addr_;
};

ClassicResolver::Impl::Impl(Config::DnsServer server, std::uint64_t id)
    : id_(id), server_(std::move(server)), uri_(Uri::parse(server_.address)), addr_(make_addr(server_)) {
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
ClassicResolver::Impl::query(const std::string &host_str, RecordKind type, int cancel_fd) const {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

        const auto record_type = DNS::Util::type_to_record_type(type);
        SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})",
                     id_, host_str, static_cast<std::uint16_t>(record_type), uri_.get_host_literal(), server_.port
        );

        // Build query packet using the native wire-format builder.
        auto query_packet = DNS::mkquery_native(host_str, record_type);

        // Try UDP first.
        // query_udp returns std::expected for I/O errors.  Socket constructor
        // failure may throw SocketException (OS resource exhaustion).
        auto response = query_udp(addr_, query_packet, cancel_fd, id_);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }

        auto resp_data = std::move(*response);

        // Validator returns std::expected for protocol violations.
        {
            auto valid = DNS::Validator::validate_response(query_packet, resp_data);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }
        }

        // Fall back to TCP if response is truncated.
        if (is_truncated(resp_data)) {
            SPDLOG_TRACE(R"(Resolver #{} UDP response truncated for "{}", falling back to TCP)", id_, host_str);
            auto tcp_response = query_tcp(addr_, query_packet, cancel_fd, id_);
            if (!tcp_response) {
                return std::unexpected(std::move(tcp_response.error()));
            }
            auto tcp_data = std::move(*tcp_response);
            {
                auto valid = DNS::Validator::validate_response(query_packet, tcp_data);
                if (!valid) {
                    return std::unexpected(std::move(valid.error()));
                }
            }
            return tcp_data;
        }

        return resp_data;
    } catch (const DnsLookupException &e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const DnsPacketException &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format(R"(Query packet construction for "{}" failed: {})", host_str, e.what())
        });
    } catch (const SocketException &e) {
        const auto err_code = DnsError::CONNECTION;
        return std::unexpected(DnsErrorInfo{
            err_code,
            fmt::format(R"(Classic resolver query for "{}" failed: {})", host_str, e.what())
        });
    } catch (const std::exception &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::UNKNOWN,
            fmt::format(R"(Classic resolver query for "{}" failed: {})", host_str, e.what())
        });
    }
}

ClassicResolver::ClassicResolver(Config::DnsServer server) : impl_(
    std::make_unique<Impl>(std::move(server), get_id())) {
}

ClassicResolver::~ClassicResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> ClassicResolver::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _classic(
        "", [](const Config::DnsServer &server) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<ClassicResolver>(server);
        });
} // namespace
