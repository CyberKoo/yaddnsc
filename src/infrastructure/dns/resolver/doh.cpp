#include "doh.h"

#include <algorithm>
#include <chrono>
#include <compare>
#include <map>
#include <new>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — coroutine lowering

#include <expected>
#include <spdlog/spdlog.h>

#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/query_budget.hpp"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/persistent_client.h"
#include "infrastructure/http/types.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"
#include "support/util/random.hpp"
#include "yaddnsc/util/format.hpp"
#include "yaddnsc/util/string_util.hpp"

namespace dns {
namespace {
constexpr unsigned char ALPN_HTTP_1_1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] domain::DnsErrorInfo map_http_error(const http::Error& error) {
    switch (error.code) {
        case http::ErrorCode::RESPONSE_PARSE_FAILED:
        case http::ErrorCode::UNSUPPORTED_PROTOCOL:
        case http::ErrorCode::HEADERS_TOO_LARGE:
        case http::ErrorCode::BODY_TOO_LARGE:
        case http::ErrorCode::INVALID_REQUEST:
        case http::ErrorCode::REDIRECT_LIMIT_EXCEEDED:
            return {domain::DnsError::PARSE, error.message};
        case http::ErrorCode::QUEUE_TIMEOUT:
            throw std::logic_error("DoH session admission unexpectedly timed out");
        case http::ErrorCode::INVALID_URL:
            return {domain::DnsError::CONFIG, error.message};
        default:
            return {domain::DnsError::CONNECTION, error.message};
    }
}

[[nodiscard]] std::string endpoint_target(const std::string& url) {
    const auto parsed = Uri::parse(url);
    if (!parsed || parsed->get_schema() != "https" || parsed->get_host().empty() || parsed->get_port() == 0) {
        throw std::invalid_argument("DoH endpoint must be a valid HTTPS URL with a nonzero port");
    }
    return http::make_target(*parsed);
}
}  // namespace

DohResolver::DohResolver(const std::string& url, http::Options options, Policy policy)
    : target_(endpoint_target(url)), policy_(policy) {
    options.tls.alpn_proto = ALPN_HTTP_1_1;
    // Local safety policy: only query the configured URI; reject all redirects.
    options.follow_redirects = false;
    options.limits.max_body_bytes = std::min(options.limits.max_body_bytes, MAX_RESPONSE_SIZE);
    if (options.connect_timeout.count() <= 0 || options.connect_timeout > policy_.connect_budget) {
        options.connect_timeout = policy_.connect_budget;
    }
    // Admission waits on our own mutex, before the backend attempt starts.
    options.queue_timeout = std::chrono::milliseconds::zero();
    client_ = std::make_unique<http::PersistentClient>(url, std::move(options));
}

DohResolver::~DohResolver() = default;

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DohResolver::query(
    std::string host, const domain::RecordKind kind) {
    auto guard = co_await mutex_.lock();
    co_return co_await detail::bounded_query([this, &host, kind]() { return query_once(host, kind); }, policy_);
}

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> DohResolver::query_once(
    std::string host, const domain::RecordKind kind) {
    try {
        const auto query_bytes = dns::build_query(host, Util::type_to_record_type(kind));
        http::Request request;
        request.method = http::Method::POST;
        request.replay_safe = true;  // DNS queries are read-only, including their HTTP POST encoding.
        request.content_type = "application/dns-message";
        request.headers.emplace("Accept", "application/dns-message");
        request.set_body(query_bytes);

        // Only a lost reused connection may be repaired/replayed here, once
        // within this attempt's remaining budget. Dispatcher owns whole-query retries.
        auto response = co_await client_->exchange(target_, request);
        if (!response)
            co_return std::unexpected(map_http_error(response.error()));
        if (response->status_ < 200 || response->status_ >= 300) {
            co_return std::unexpected(domain::DnsErrorInfo{
                response->status_ >= 500 || response->status_ == 429 ? domain::DnsError::RETRY
                                                                     : domain::DnsError::SERVER_REFUSED,
                fmt::format("DoH endpoint returned HTTP status {}", response->status_)});
        }
        std::string_view content_type;
        for (const auto& [name, value] : response->headers_) {
            if (StringUtil::iequals(name, "content-type")) {
                content_type = value;
                break;
            }
        }
        const auto media_type = StringUtil::trim(content_type.substr(0, content_type.find(';')));
        if (!StringUtil::iequals(media_type, "application/dns-message")) {
            // RFC 8484 §5.4: the endpoint answered, but not with DNS. Report
            // what it actually sent — a captive portal's text/html or an
            // absent header is the whole diagnosis.
            co_return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::PARSE,
                                     fmt::format(R"(DoH endpoint returned a missing or unexpected Content-Type: "{}")",
                                                 content_type.empty() ? "<missing>" : content_type)});
        }
        const auto octets = response->bytes();
        std::vector<std::uint8_t> body(octets.begin(), octets.end());
        if (auto valid = Validator::validate_response(query_bytes, body); !valid) {
            co_return std::unexpected(std::move(valid.error()));
        }
        if (detail::is_truncated(body)) {
            co_return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::PARSE, "DoH endpoint returned a truncated (TC) answer"});
        }
        SPDLOG_TRACE(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), body.size(), host);
        co_return body;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const Utils::Random::EntropyError& error) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, error.what()});
    } catch (const DnsPacketException& error) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY,
                                 fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    }
}

void DohResolver::close() noexcept {
    if (client_ != nullptr)
        client_->close();
}
}  // namespace dns
