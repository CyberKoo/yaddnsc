//
// dns — DNS over HTTPS.
//

#include "doh.h"

#include <spdlog/spdlog.h>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <chrono>
#include <cstdint>
#include <expected>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <map>
#include <stdexcept>

#include "coro/scope.hpp"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/persistent_client.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/options.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace dns {
namespace {

/// ALPN identifier for HTTP/1.1 (RFC 7301) — DoH uses HTTP/1.1 here.
constexpr unsigned char ALPN_HTTP_1_1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

/// Per-attempt exchange budget: a stalled endpoint fails as CONNECTION
/// (retryable at the dispatcher level) instead of parking until the caller's
/// scope fires. 1s keeps one attempt — two with the retry — under the caller's
/// DNS read budget (DNS_READ_BUDGET, 4s). The connect phase carries its own
/// budget instead: Options::connect_timeout, which the DoH factory sets to 1s
/// (the http::Options default is 5s).
constexpr auto EXCHANGE_BUDGET = std::chrono::seconds(1);

/// Map an HTTP failure to the DNS vocabulary.
///
/// The split follows `detail::is_definitive` (PARSE, CONFIG) and
/// `detail::is_retryable` (RETRY, CONNECTION): a defect on this side of the
/// socket cannot be fixed by asking again, while a property of one endpoint's
/// response must not end a search another backend could still answer.
///
/// - CONFIG — the endpoint as configured cannot serve this client at all: an
///   unparsable URL, or a protocol upgrade it insists on (HTTP 101). The
///   outcome is fixed for the endpoint, so the dispatcher stops instead of
///   burning retries on it.
/// - PARSE — the exchange is malformed and stays that way: a response that
///   cannot be framed or parsed, or a request this resolver builds being
///   rejected (malformed target or header, too many redirects). Deterministic
///   in this process, so a retry cannot succeed; REDIRECT_LIMIT is unreachable
///   while redirects are off, but stays here so the classification holds if
///   that policy changes.
/// - RETRY — the endpoint answered, but with something this client refuses
///   (oversized headers or body). Transient for the DNS search: the dispatcher
///   should try the next backend instead of abandoning the name.
/// - CONNECTION — the transport never produced a usable exchange, so the same
///   query may still succeed here or elsewhere.
[[nodiscard]] domain::DnsErrorInfo map_http_error(const http::Error& error) {
    switch (error.code) {
        case http::ErrorCode::RESPONSE_PARSE_FAILED:
            return domain::DnsErrorInfo{domain::DnsError::PARSE, error.message};
        case http::ErrorCode::HEADERS_TOO_LARGE:
        case http::ErrorCode::BODY_TOO_LARGE:
            return domain::DnsErrorInfo{domain::DnsError::RETRY, error.message};
        case http::ErrorCode::RESOLVE_FAILED:
        case http::ErrorCode::CONNECT_FAILED:
        case http::ErrorCode::TLS_HANDSHAKE_FAILED:
        case http::ErrorCode::CONNECTION_LOST:
            return domain::DnsErrorInfo{domain::DnsError::CONNECTION, error.message};
        case http::ErrorCode::INVALID_URL:
        case http::ErrorCode::UNSUPPORTED_PROTOCOL:
            return domain::DnsErrorInfo{domain::DnsError::CONFIG, error.message};
        case http::ErrorCode::INVALID_REQUEST:
        case http::ErrorCode::REDIRECT_LIMIT_EXCEEDED:
            return domain::DnsErrorInfo{domain::DnsError::PARSE, error.message};
        default:
            // An unrecognised code is read as transient: calling a live failure
            // definitive would abandon a name another backend can still answer,
            // while the reverse only costs one wasted round trip.
            return domain::DnsErrorInfo{domain::DnsError::CONNECTION, error.message};
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
    // RFC 8484 §3: a DoH client must not use a URI it was not configured with,
    // so redirects are never followed — a redirect is reported as a failure
    // instead of silently querying another host.
    options.follow_redirects = false;
    client_ = std::make_shared<http::PersistentClient>(std::move(url), std::move(options));
    label_ = fmt::format("{}://{}:{}", client_->scheme(), client_->host(), client_->port());
}

DohResolver::~DohResolver() = default;

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DohResolver::query(
    std::string host, const domain::RecordKind kind) {
    try {
        const auto record_type = dns::Util::type_to_record_type(kind);
        SPDLOG_TRACE(R"(Resolver #{} lookup for domain "{}" (type {}))", id(), host,
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
                [this, &target = target_, &request]() -> coro::Task<std::expected<http::Response, http::Error>> {
                    co_return co_await client_->exchange(target, request);
                });
            if (exchanged.timed_out) {
                // Own budget fired; ancestor cancellation propagates:
                // the legacy transport-timeout path, mapped to CONNECTION. The
                // cancelled exchange already dropped the connection inside the
                // session, so a retry reconnects on its own.
                if (attempt + 1 < MAX_ATTEMPTS) {
                    continue;  // rebuild once, then give up
                }
                co_return std::unexpected(
                    domain::DnsErrorInfo{domain::DnsError::CONNECTION,
                                         fmt::format(R"(Failed to read response from "{}": timed out)", label_)});
            }
            auto& response = *exchanged;
            if (!response) {
                // A failed exchange never leaves the connection behind: the
                // session dropped it already.
                if (attempt + 1 < MAX_ATTEMPTS) {
                    continue;  // rebuild once, then give up
                }
                co_return std::unexpected(map_http_error(response.error()));
            }

            // RFC 8484 §4.2.1: any 2xx status carries a usable answer. Any other
            // status is a per-query failure — the shared connection is healthy
            // and stays open for the sibling queries.
            if (response->status_ < 200 || response->status_ >= 300) {
                co_return std::unexpected(domain::DnsErrorInfo{
                    response->status_ >= 500 ? domain::DnsError::RETRY : domain::DnsError::SERVER_REFUSED,
                    fmt::format("DoH endpoint returned HTTP status {}", response->status_)});
            }

            // RFC 8484 §5.4: the payload must be a DNS message — a wrong or
            // missing Content-Type means the endpoint did not answer with DNS.
            std::string_view content_type;
            for (const auto& [name, value] : response->headers_) {
                if (StringUtil::iequals(name, "content-type")) {
                    content_type = value;
                    break;
                }
            }
            const auto media_type = StringUtil::trim(content_type.substr(0, content_type.find(';')));
            if (!StringUtil::iequals(media_type, "application/dns-message")) {
                co_return std::unexpected(domain::DnsErrorInfo{
                    domain::DnsError::PARSE,
                    fmt::format(R"(DoH endpoint returned an unexpected Content-Type: "{}")",
                                content_type.empty() ? "<missing>" : content_type)});
            }

            const auto octets = response->bytes();
            std::vector<std::uint8_t> body(octets.begin(), octets.end());
            if (auto valid = dns::Validator::validate_response(query_bytes, body); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            if (detail::is_truncated(body)) {
                // DoH has no TCP fallback, so a truncated answer must not be
                // parsed as if complete (RFC 8484 §10) — it is transient, and
                // the dispatcher can fail over to another backend.
                co_return std::unexpected(
                    domain::DnsErrorInfo{domain::DnsError::RETRY, "DoH endpoint returned a truncated (TC) answer"});
            }
            SPDLOG_TRACE(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), body.size(), host);
            co_return body;
        }
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DoH query failed"});
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{error.get_error(), error.what()});
    } catch (const DnsPacketException& error) {
        // Packet construction failure is a definitive input defect (e.g. an
        // over-long name), never a transient transport error (see ClassicResolver).
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    } catch (const std::exception& error) {
        // Nothing in this path is expected to throw, so log the exception that
        // was not handled by a catch above before it is downgraded.
        SPDLOG_WARN(R"(Resolver #{} DoH query for "{}" raised an unexpected exception: {})", id(), host, error.what());
        // See ClassicResolver: unexpected exceptions stay retryable as UNKNOWN.
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::UNKNOWN, fmt::format(R"(DoH query for "{}" failed: {})", host, error.what())});
    }
}

void DohResolver::close() noexcept {
    if (client_ != nullptr) {
        client_->close();
    }
}

}  // namespace dns
