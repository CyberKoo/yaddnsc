//
// ClassicResolver: UDP query plus TCP fallback.
//
// A resolver facade, above the transport layer, so the TCP fallback can use
// TcpStream. The byte-level exchanges it shares with bootstrap live below the
// transport layer beside bootstrap.cpp (classic_udp, classic_tcp); the
// framing both TCP paths need lives in wire/framing.h.
//
// This translation unit is the upward edge and is not part of
// yaddnsc_dns_classic.
//
#include "classic.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/classic/classic_udp.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/network/socket_addr.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/uri.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace {

constexpr auto UDP_BUDGET = std::chrono::seconds(1);
constexpr auto TCP_OP_BUDGET = std::chrono::seconds(1);

[[nodiscard]] SocketAddr make_addr(const Config::DnsServer& server) {
    auto parsed = InetAddress::parse(server.address);
    if (!parsed) {
        throw DnsLookupException(
            fmt::format(R"(Invalid DNS server address "{}" — must be an IP address)", server.address),
            DnsError::CONFIG);
    }

    auto sa = SocketAddr::from_inet(*parsed, server.port);
    if (!sa) {
        throw DnsLookupException(fmt::format(R"(Failed to build socket address for "{}")", server.address),
                                 DnsError::CONFIG);
    }

    return *sa;
}

/// Parse the server address into a Uri (used for display). Same error
/// contract as make_addr(): a malformed address is a configuration
/// terminate-signal, reported as DnsLookupException with DnsError::CONFIG.
[[nodiscard]] Uri parse_server_uri(const Config::DnsServer& server) {
    auto uri = Uri::parse(server.address);
    if (!uri.has_value()) {
        throw DnsLookupException(
            fmt::format(R"(Invalid DNS server address "{}" ({}))", server.address, error_message(uri.error())),
            DnsError::CONFIG);
    }
    return std::move(*uri);
}

[[nodiscard]] DnsError map_io(const Transport::IoError err) {
    switch (err) {
        case Transport::IoError::TIMEOUT:
            return DnsError::RETRY;
        case Transport::IoError::CANCELLED:
            return DnsError::CANCELLED;
        case Transport::IoError::CONNECTION_FAILED:
            return DnsError::CONNECTION;
    }
    return DnsError::CONNECTION;
}

[[nodiscard]] bool is_truncated(const std::vector<std::uint8_t>& response) {
    return response.size() >= DNS::HEADER_SIZE && (response[2] & 0x02) != 0;
}

/// TCP fallback. Each stream call has its own one-second budget: connect,
/// the length-prefixed write, and the exact read do not share one clock.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query_tcp(
    const Config::DnsServer& server, const std::span<const std::uint8_t> query_packet,
    const Utils::CancellationToken& token, const std::uint64_t resolver_id) {
    Transport::Options opts;
    opts.connect_timeout = TCP_OP_BUDGET;
    opts.read_timeout = TCP_OP_BUDGET;
    opts.write_timeout = TCP_OP_BUDGET;

    Transport::TcpStream stream(server.address, server.port, std::move(opts));
    if (auto connected = stream.ensure_connected(token); !connected) {
        return std::unexpected(
            DnsErrorInfo{map_io(connected.error()), fmt::format(R"(Resolver #{} TCP connect failed)", resolver_id)});
    }

    auto framed = DNS::frame_message(query_packet);
    if (!framed) {
        return std::unexpected(
            DnsErrorInfo{DnsError::PARSE, fmt::format(R"(Resolver #{} TCP query exceeds 65535 bytes)", resolver_id)});
    }

    if (auto sent = stream.send_all(*framed, token); !sent) {
        return std::unexpected(
            DnsErrorInfo{map_io(sent.error()), fmt::format(R"(Resolver #{} TCP send failed)", resolver_id)});
    }

    std::array<std::uint8_t, 2> len_buf{};
    if (auto got = stream.read_exact(len_buf, token); !got) {
        return std::unexpected(
            DnsErrorInfo{map_io(got.error()), fmt::format(R"(Resolver #{} TCP recv failed)", resolver_id)});
    }

    const auto rsp_len = DNS::read_length(std::span{len_buf});
    if (!rsp_len) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format("Invalid DNS response length: {}", DNS::announced_length(std::span{len_buf}))});
    }

    std::vector<std::uint8_t> response(*rsp_len);
    if (auto got = stream.read_exact(response, token); !got) {
        return std::unexpected(
            DnsErrorInfo{map_io(got.error()), fmt::format(R"(Resolver #{} TCP recv failed)", resolver_id)});
    }
    return response;
}

}  // namespace

struct ClassicResolver::Impl {
    explicit Impl(Config::DnsServer server, std::uint64_t id);

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(
        const std::string& host_str, RecordKind type, const Utils::CancellationToken& token) const;

    std::uint64_t id_;
    Config::DnsServer server_;
    Uri uri_;
    SocketAddr addr_;
};

ClassicResolver::Impl::Impl(Config::DnsServer server, const std::uint64_t id)
    : id_(id), server_(std::move(server)), uri_(parse_server_uri(server_)), addr_(make_addr(server_)) {}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> ClassicResolver::Impl::query(
    const std::string& host_str, const RecordKind type, const Utils::CancellationToken& token) const {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

        const auto record_type = DNS::Util::type_to_record_type(type);
        SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id_, host_str,
                     static_cast<std::uint16_t>(record_type), uri_.get_host_literal(), server_.port);

        auto query_packet = DNS::build_query(host_str, record_type);
        const auto deadline = std::chrono::steady_clock::now() + UDP_BUDGET;
        auto response = DNS::exchange_udp(addr_, query_packet, deadline, token, id_);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }

        auto resp_data = std::move(*response);
        if (auto valid = DNS::Validator::validate_response(query_packet, resp_data); !valid) {
            return std::unexpected(std::move(valid.error()));
        }

        if (is_truncated(resp_data)) {
            SPDLOG_TRACE(R"(Resolver #{} UDP response truncated for "{}", falling back to TCP)", id_, host_str);
            auto tcp_response = query_tcp(server_, query_packet, token, id_);
            if (!tcp_response) {
                return std::unexpected(std::move(tcp_response.error()));
            }
            auto tcp_data = std::move(*tcp_response);
            if (auto valid = DNS::Validator::validate_response(query_packet, tcp_data); !valid) {
                return std::unexpected(std::move(valid.error()));
            }
            return tcp_data;
        }

        return resp_data;
    } catch (const DnsLookupException& e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const DnsPacketException& e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE, fmt::format(R"(Query packet construction for "{}" failed: {})", host_str, e.what())});
    } catch (const std::exception& e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::UNKNOWN, fmt::format(R"(Classic resolver query for "{}" failed: {})", host_str, e.what())});
    }
}

ClassicResolver::ClassicResolver(Config::DnsServer server)
    : impl_(std::make_unique<Impl>(std::move(server), get_id())) {}

ClassicResolver::~ClassicResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> ClassicResolver::query(
    const std::string& host, const RecordKind type, const Utils::CancellationToken& token) const {
    return impl_->query(host, type, token);
}
