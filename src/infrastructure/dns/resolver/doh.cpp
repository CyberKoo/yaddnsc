//
// Created by Kotarou on 2026/6/28.
//
// DNS-over-HTTPS resolver (RFC 8484) on Transport + net::http.
//

#include "doh.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <expected>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/network/transport/tls_stream.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

#include "version.h"

enum class RecordKind;

namespace {
using namespace std::chrono_literals;

/// Map a transport I/O error to DnsErrorInfo (connect stage).
[[nodiscard]] DnsErrorInfo map_connect_error(const Transport::IoError err, const std::string_view label) {
    using enum Transport::IoError;
    switch (err) {
        case CANCELLED:
            return {DnsError::CANCELLED, "Query cancelled"};
        case TIMEOUT:
            return {DnsError::RETRY, fmt::format(R"(Connection to "{}" timed out)", label)};
        case CONNECTION_FAILED:
            return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
    }
    return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
}

/// Map an HTTP protocol error to DnsErrorInfo (exchange stage).
[[nodiscard]] DnsErrorInfo map_http_error(const net::http::Error& err, const std::string_view label) {
    switch (err.code) {
        case net::http::ErrorCode::CANCELLED:
            return {DnsError::CANCELLED, "Query cancelled"};
        case net::http::ErrorCode::TIMEOUT:
        case net::http::ErrorCode::CONNECT_FAILED:
        case net::http::ErrorCode::TLS_HANDSHAKE_FAILED:
        case net::http::ErrorCode::CONNECTION_LOST:
            return {DnsError::CONNECTION, fmt::format(R"(Failed to read response from "{}": {})", label, err.message)};
        case net::http::ErrorCode::RESPONSE_PARSE_FAILED:
        case net::http::ErrorCode::HEADERS_TOO_LARGE:
        case net::http::ErrorCode::BODY_TOO_LARGE:
            return {DnsError::PARSE,
                    fmt::format(R"(Server "{}" returned malformed HTTP response: {})", label, err.message)};
        default:
            return {DnsError::CONNECTION, fmt::format(R"(HTTP query to "{}" failed: {})", label, err.message)};
    }
}

/// Build a proper HTTP Host header value per RFC 7230 §5.4.
/// Omits the port when it is the HTTPS default (443) and brackets IPv6.
[[nodiscard]] std::string build_host_header(const std::string_view host, const std::uint16_t port) {
    const bool is_ipv6 = host.find(':') != std::string_view::npos;
    if (port == 443) {
        return is_ipv6 ? fmt::format("[{}]", host) : std::string(host);
    }
    return is_ipv6 ? fmt::format("[{}]:{}", host, port) : fmt::format("{}:{}", host, port);
}
}  // namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

struct DohResolver::Impl {
    // ── Constants ──
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_HTTP[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

    /// Connection + TLS options for the DoH connection.
    [[nodiscard]] static std::pair<Transport::Options, Transport::TlsOptions> make_tls_options() {
        Transport::Options conn;
        conn.connect_timeout = CONNECT_TIMEOUT;
        Transport::TlsOptions tls;
        tls.alpn_proto = ALPN_HTTP;
        return {conn, tls};
    }

    /// Production ctor: creates the TLS stream with the token bound.
    Impl(std::string server,
         std::uint16_t port,
         std::string path,
         std::uint64_t id,
         std::string label,
         Utils::CancellationToken token);

    /// Testing ctor: stream injected.
    Impl(std::string server,
         std::uint16_t port,
         std::string path,
         std::uint64_t id,
         std::string label,
         std::unique_ptr<Transport::Stream> stream);

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(const std::string& host,
                                                                               RecordKind type) const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string host_;
    const std::uint16_t port_;
    const std::string path_;
    const std::string host_header_;
    const std::string label_;  // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Transport::Stream> stream_;
};

DohResolver::Impl::Impl(std::string server,
                        const std::uint16_t port,
                        std::string path,
                        const std::uint64_t id,
                        std::string label,
                        Utils::CancellationToken token)
    : id_(id), host_(std::move(server)), port_(port), path_(std::move(path)),
      host_header_(build_host_header(host_, port_)), label_(std::move(label)),
      stream_(std::make_unique<Transport::TlsStream>(host_,
                                                     port_,
                                                     make_tls_options().first,
                                                     make_tls_options().second,
                                                     std::move(token))) {}

DohResolver::Impl::Impl(std::string server,
                        const std::uint16_t port,
                        std::string path,
                        const std::uint64_t id,
                        std::string label,
                        std::unique_ptr<Transport::Stream> stream)
    : id_(id), host_(std::move(server)), port_(port), path_(std::move(path)),
      host_header_(build_host_header(host_, port_)), label_(std::move(label)), stream_(std::move(stream)) {}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::Impl::query(const std::string& host,
                                                                                RecordKind type) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the raw DNS query packet ----
        const auto query_bytes = DNS::build_query(host, record_type);

        // ---- 2. Build the HTTP/1.1 POST request (RFC 8484) ----
        net::http::protocol::WireRequest req{
            .method = net::http::Method::POST,
            .target = path_,
            .headers =
                {
                    {"Host", host_header_},
                    {"User-Agent", std::string(YADDNSC::get_full_version())},
                    {"Accept", "application/dns-message"},
                    {"Content-Type", "application/dns-message"},
                    {"Content-Length", std::to_string(query_bytes.size())},
                },
            .body = std::nullopt,
        };
        req.body.emplace(reinterpret_cast<const char*>(query_bytes.data()), query_bytes.size());

        // ---- 3. Exchange over the persistent stream, one rebuild-retry ----
        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            std::lock_guard lock(mutex_);

            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
                stream_->close();
            }

            // ensure_connected() is idempotent: healthy → no-op, stale → rebuild.
            if (auto connected = stream_->ensure_connected(); !connected) {
                stream_->close();
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(map_connect_error(connected.error(), label_));
            }

            auto response = net::http::protocol::exchange(*stream_, req, {});
            if (!response) {
                stream_->close();
                if (response.error().code == net::http::ErrorCode::CANCELLED) {
                    return std::unexpected(map_http_error(response.error(), label_));
                }
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(map_http_error(response.error(), label_));
            }

            // Only HTTP 200 is a valid DoH response (RFC 8484 §4.2.1).
            if (response->status != 200) {
                stream_->close();
                return std::unexpected(
                    DnsErrorInfo{response->status >= 500 ? DnsError::RETRY : DnsError::SERVER_REFUSED,
                                 fmt::format(R"(Server "{}" returned HTTP status {})", label_, response->status)});
            }

            // ---- 4. Validate the DNS response header (RFC 8484 §5.1 / RFC 1035 §4.1.1) ----
            const auto octets = response->bytes();
            const std::vector<std::uint8_t> body(octets.begin(), octets.end());
            auto valid = DNS::Validator::validate_response(query_bytes, body);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }

            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}"))", id_, body.size(), host);

            return body;
        }

        // Not reached.
        std::unreachable();
    } catch (const DnsLookupException& e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const std::exception& e) {
        return std::unexpected(
            DnsErrorInfo{DnsError::UNKNOWN, fmt::format(R"(Query for "{}" failed: {})", host, e.what())});
    }
}

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::string host,
                         const std::uint16_t port,
                         std::string path,
                         std::string label,
                         Utils::CancellationToken token)
    : impl_(std::make_unique<Impl>(std::move(host),
                                   port,
                                   std::move(path),
                                   get_id(),
                                   std::move(label),
                                   std::move(token))) {}

DohResolver::DohResolver(std::string host,
                         const std::uint16_t port,
                         std::string path,
                         std::string label,
                         std::unique_ptr<Transport::Stream> stream)
    : impl_(std::make_unique<Impl>(std::move(host),
                                   port,
                                   std::move(path),
                                   get_id(),
                                   std::move(label),
                                   std::move(stream))) {}

DohResolver::~DohResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::query(const std::string& host,
                                                                          RecordKind type) const {
    return impl_->query(host, type);
}
