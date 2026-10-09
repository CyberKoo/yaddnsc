//
// dns — DNS over HTTPS.
//

#include "doh.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "infrastructure/coro/scope.hpp"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/net/http/client.h"
#include "infrastructure/net/http/wire_request.h"
#include "infrastructure/net/http/uri.h"
#include "support/fmt.hpp"

namespace dns {
namespace {

/// ALPN identifier for HTTP/1.1 (RFC 7301) — DoH uses HTTP/1.1 here.
constexpr unsigned char ALPN_HTTP_1_1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

/// Per-attempt exchange budget, replacing the legacy per-operation transport
/// timeouts (1s connect + 5s send/read): a stalled endpoint fails as
/// CONNECTION (retryable at the dispatcher level) instead of parking until
/// the caller's scope fires.
constexpr auto EXCHANGE_BUDGET = std::chrono::seconds(10);

/// Map an HTTP failure to the DNS vocabulary.
[[nodiscard]] DnsErrorInfo map_http_error(const http::Error& error) {
    switch (error.code) {
        case http::ErrorCode::CANCELLED:
            return DnsErrorInfo{DnsError::CANCELLED, error.message};
        case http::ErrorCode::RESPONSE_PARSE_FAILED:
        case http::ErrorCode::HEADERS_TOO_LARGE:
        case http::ErrorCode::BODY_TOO_LARGE:
            return DnsErrorInfo{DnsError::PARSE, error.message};
        case http::ErrorCode::RESOLVE_FAILED:
        case http::ErrorCode::CONNECT_FAILED:
        case http::ErrorCode::TLS_HANDSHAKE_FAILED:
        case http::ErrorCode::CONNECTION_LOST:
            return DnsErrorInfo{DnsError::CONNECTION, error.message};
        default:
            return DnsErrorInfo{DnsError::CONNECTION, error.message};
    }
}

/// The request path of a DoH endpoint URL.
[[nodiscard]] std::string endpoint_target(const std::string& url) {
    const auto parsed = Uri::parse(url);
    if (!parsed.has_value()) {
        return {};
    }
    auto target = std::string(parsed->get_path());
    if (target.empty()) {
        target = "/";
    }
    if (const auto query = parsed->get_query_string(); !query.empty()) {
        target += '?';
        target += query;
    }
    return target;
}

}  // namespace

DohResolver::DohResolver(std::string url, http::Options options) : target_(endpoint_target(url)), client_(nullptr) {
    if (target_.empty()) {
        throw std::invalid_argument(fmt::format(R"(invalid DoH endpoint URL: "{}")", url));
    }
    options.tls.alpn_proto = ALPN_HTTP_1_1;
    client_ = std::make_shared<http::PersistentClient>(std::move(url), std::move(options));
    label_ = fmt::format("{}://{}:{}", client_->scheme(), client_->host(), client_->port());
}

DohResolver::~DohResolver() = default;

coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> DohResolver::query(std::string host,
                                                                                      const RecordKind kind) {
    try {
        const auto record_type = dns::Util::type_to_record_type(kind);
        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id(), host,
                     static_cast<std::uint16_t>(record_type));

        const auto query_bytes = dns::build_query(host, record_type);

        http::Request request;
        request.method = http::Method::POST;
        request.content_type = "application/dns-message";
        request.headers.emplace("Accept", "application/dns-message");
        request.set_body(query_bytes);

        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
            }
            auto exchanged = co_await coro::with_timeout(
                EXCHANGE_BUDGET,
                [this, &target = target_, &request](
                    coro::CancelScope&) -> coro::Task<std::expected<http::Response, http::Error>> {
                    co_return co_await client_->exchange(target, request);
                });
            if (exchanged.timed_out) {
                // Own budget fired (checked before the body's CANCELLED value):
                // the legacy transport-timeout path, mapped to CONNECTION. The
                // cancelled exchange already dropped the connection inside the
                // session, so a retry reconnects on its own.
                if (attempt + 1 < MAX_ATTEMPTS) {
                    continue;  // rebuild once, then give up
                }
                co_return std::unexpected(
                    DnsErrorInfo{DnsError::CONNECTION,
                                 fmt::format(R"(Failed to read response from "{}": timed out)", label_)});
            }
            auto& response = *exchanged;
            if (!response) {
                if (response.error().code == http::ErrorCode::CANCELLED) {
                    co_return std::unexpected(map_http_error(response.error()));
                }
                // A failed exchange never leaves the connection behind: the
                // session dropped it already.
                if (attempt + 1 < MAX_ATTEMPTS) {
                    continue;  // rebuild once, then give up
                }
                co_return std::unexpected(map_http_error(response.error()));
            }

            // RFC 8484 §4.2.1: only 200 carries a usable answer. Any other
            // status is a per-query failure — the shared connection is healthy
            // and stays open for the sibling queries.
            if (response->status != 200) {
                co_return std::unexpected(
                    DnsErrorInfo{response->status >= 500 ? DnsError::RETRY : DnsError::SERVER_REFUSED,
                                 fmt::format("DoH endpoint returned HTTP status {}", response->status)});
            }

            const auto octets = response->bytes();
            std::vector<std::uint8_t> body(octets.begin(), octets.end());
            if (auto valid = dns::Validator::validate_response(query_bytes, body); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), body.size(), host);
            co_return body;
        }
        co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DoH query failed"});
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(DnsErrorInfo{error.get_error(), error.what()});
    } catch (const std::exception& error) {
        co_return std::unexpected(
            DnsErrorInfo{DnsError::PARSE, fmt::format(R"(DoH query for "{}" failed: {})", host, error.what())});
    }
}

void DohResolver::close() noexcept {
    if (client_ != nullptr) {
        client_->close();
    }
}

}  // namespace dns
