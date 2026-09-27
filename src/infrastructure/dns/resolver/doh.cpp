//
// Created by Kotarou on 2026/6/28.
//
// DNS-over-HTTPS resolver (RFC 8484) on Transport + net::http.
//

#include "doh.h"

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
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
#include "infrastructure/dns/resolver/connect_error.hpp"
#include "infrastructure/dns/resolver/tls_options.hpp"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

#include "version.h"

enum class RecordKind;

namespace {
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

constexpr unsigned char ALPN_HTTP[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
}  // namespace

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(DohEndpoint endpoint, std::vector<Config::DnsServer> bootstrap)
    : id_(get_id()), host_(std::move(endpoint.host)), port_(endpoint.port), path_(std::move(endpoint.path)),
      host_header_(build_host_header(host_, port_)), label_(std::move(endpoint.label)),
      bootstrap_(std::move(bootstrap)), stream_(DNS::Resolver::make_tls_stream(host_, port_, bootstrap_, ALPN_HTTP)) {}

DohResolver::DohResolver(DohEndpoint endpoint, std::unique_ptr<Transport::Stream> stream)
    : id_(get_id()), host_(std::move(endpoint.host)), port_(endpoint.port), path_(std::move(endpoint.path)),
      host_header_(build_host_header(host_, port_)), label_(std::move(endpoint.label)), bootstrap_{},
      stream_(std::move(stream)) {}

DohResolver::~DohResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::query(const std::string& host, RecordKind type,
                                                                          const Utils::CancellationToken& token) const {
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
            if (auto connected = stream_->ensure_connected(token); !connected) {
                stream_->close();
                if (connected.error() == Transport::IoError::CANCELLED) {
                    return std::unexpected(DNS::Resolver::map_connect_error(connected.error(), label_));
                }
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(DNS::Resolver::map_connect_error(connected.error(), label_));
            }

            auto response = net::http::protocol::exchange(*stream_, req, {}, token);
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

            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id_, body.size(), host);

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
