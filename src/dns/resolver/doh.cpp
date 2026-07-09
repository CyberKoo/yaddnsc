//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"

#include "dns/resolver_registry.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/query.h"
#include "exception/dns_lookup.h"
#include "network/http_client.h"

#include "dns_error.h"
#include "http_type.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
    constexpr auto DOH_CONTENT_TYPE = "application/dns-message";
} // anonymous namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

struct DohResolver::Impl {
    // ── Constants ──
    static constexpr size_t MAX_DNS_RESPONSE_SIZE = 65536;

    // ── Constructor ──
    explicit Impl(std::unique_ptr<HttpClient> http_client, std::string server, std::uint64_t id);

    // ── Member functions ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsLookupException> query(
        const std::string &host, RecordKind type, int cancel_fd = -1) const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    std::unique_ptr<HttpClient> http_client_;
};

DohResolver::Impl::Impl(std::unique_ptr<HttpClient> http_client, std::string server, std::uint64_t id)
    : id_(id), server_(std::move(server)), http_client_(std::move(http_client)) {
}

std::expected<std::vector<std::uint8_t>, DnsLookupException>
DohResolver::Impl::query(const std::string &host, RecordKind type, [[maybe_unused]] int cancel_fd) const {
    try {
        // NOT IMPLEMENTED: httplib::Client provides no cancellation API.
        // To add support, HttpClient must provide its own cancellation mechanism.
        // See: classic_native.cpp (reference impl).
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        const auto query_bytes = DNS::mkquery(host, record_type);

        HttpRequest req{
            .content_type = DOH_CONTENT_TYPE,
            .method = HttpMethod::POST,
            .headers = {{"Accept", DOH_CONTENT_TYPE}},
            // DNS wire format is binary; std::string is used here as a byte container
            // not as a null-terminated C-string.
            .body = std::string(reinterpret_cast<const char *>(query_bytes.data()), query_bytes.size())
        };

        SPDLOG_TRACE(R"(DoH POST {}  ({} bytes))", server_, query_bytes.size());

        const auto response = http_client_->exchange(server_, req);

        if (!response) {
            throw DnsLookupException(
                fmt::format(R"(DoH query to "{}" failed: {})", server_, response.error()),
                DnsError::CONNECTION
            );
        }

        if (response->status_code != 200) {
            // HTTP errors don't carry DNS semantics — do NOT map 4xx to NODATA.
            // 4xx: our request was rejected (permanent, no point retrying).
            // 5xx: server-side transient failure (retry may succeed).
            const auto ec = response->status_code >= 500 ? DnsError::RETRY : DnsError::SERVER_REFUSED;
            throw DnsLookupException(
                fmt::format(R"(DoH server "{}" returned HTTP status {})", server_, response->status_code),
                ec
            );
        }

        // Validate Content-Type per RFC 8484 §6.
        {
            bool valid_ct = false;
            auto range = response->headers.equal_range("Content-Type");
            for (auto it = range.first; it != range.second && !valid_ct; ++it) {
                valid_ct = it->second.find("application/dns-message") != std::string::npos;
            }
            if (!valid_ct) {
                throw DnsLookupException(
                    fmt::format(R"(DoH server "{}" returned unexpected Content-Type)", server_),
                    DnsError::PARSE
                );
            }
        }

        // Reject oversized responses to guard against OOM.
        if (response->body.size() > MAX_DNS_RESPONSE_SIZE) {
            throw DnsLookupException(
                fmt::format(R"(DoH server "{}" returned oversized response: {} bytes)", server_, response->body.size()),
                DnsError::PARSE
            );
        }

        // Response body is DNS wire-format binary stored in std::string (see above).
        const auto *body_data = reinterpret_cast<const std::uint8_t *>(response->body.data());

        // Validate DNS response header (RFC 8484 §5.1 / RFC 1035 §4.1.1).
        DNS::Validator::validate_response(query_bytes, std::span(body_data, response->body.size()));

        SPDLOG_DEBUG(R"(Resolver #{} query for "{}" succeeded ({} bytes))", id_, host, response->body.size());

        return std::vector<std::uint8_t>(body_data, body_data + response->body.size());
    } catch (const DnsLookupException &e) {
        return std::unexpected(e);
    }
}

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::unique_ptr<HttpClient> http_client, std::string server)
    : impl_(std::make_unique<Impl>(std::move(http_client), std::move(server), get_id())) {
}

DohResolver::~DohResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsLookupException> DohResolver::query(
    const std::string &host, RecordKind type, int cancel_fd) const noexcept {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _doh(
        "https",
        [](const Config::DnsServer &server) -> std::shared_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            auto opts = HttpClientOptions{
                .connection_timeout = std::chrono::seconds(1),
                .read_timeout = std::chrono::seconds(5),
                .follow_location = false
            };
            auto http_client = std::make_unique<PersistentHttpClient>(uri, opts);
            return std::make_shared<DohResolver>(std::move(http_client), server.address);
        });
} // namespace
