//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"

#include <resolv.h>
#include <arpa/nameser.h>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "http_type.h"
#include "dns/wire/query.h"
#include "dns/validator.h"
#include "network/http_client.h"
#include "exception/dns_lookup.h"

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
    [[nodiscard]] std::vector<std::uint8_t> query(const std::string &host, DNS::Type type) const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    std::unique_ptr<HttpClient> http_client_;
};

DohResolver::Impl::Impl(std::unique_ptr<HttpClient> http_client, std::string server, std::uint64_t id) : id_(id),
    server_(std::move(server)), http_client_(std::move(http_client)) {
}

std::vector<std::uint8_t> DohResolver::Impl::query(const std::string &host, DNS::Type type) const {
    const auto ns_type_val = DNS::Util::to_ns_type(type);
    if (ns_type_val == ns_t_invalid) {
        throw DnsLookupException(
            fmt::format(R"(Unsupported DNS::Type for DoH query: "{}")", host),
            DNS::Error::UNKNOWN
        );
    }

    SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host, ns_type_val);

    const auto query_bytes = DNS::mkquery(host, ns_type_val);

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
            DNS::Error::CONNECTION
        );
    }

    if (response->status_code != 200) {
        // HTTP errors don't carry DNS semantics — do NOT map 4xx to NODATA.
        // 4xx: our request was rejected (permanent, no point retrying).
        // 5xx: server-side transient failure (retry may succeed).
        const auto ec = response->status_code >= 500 ? DNS::Error::RETRY : DNS::Error::UNKNOWN;
        throw DnsLookupException(
            fmt::format(R"(DoH server "{}" returned HTTP status {})", server_, response->status_code),
            ec);
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
                DNS::Error::PARSE);
        }
    }

    // Reject oversized responses to guard against OOM.
    if (response->body.size() > MAX_DNS_RESPONSE_SIZE) {
        throw DnsLookupException(
            fmt::format(R"(DoH server "{}" returned oversized response: {} bytes)", server_, response->body.size()),
            DNS::Error::PARSE);
    }

    // Response body is DNS wire-format binary stored in std::string (see above).
    const auto *body_data = reinterpret_cast<const std::uint8_t *>(response->body.data());

    // Validate DNS response header (RFC 8484 §5.1 / RFC 1035 §4.1.1).
    DNS::Validator::validate_response(query_bytes, std::span(body_data, response->body.size()));

    SPDLOG_DEBUG(R"(Resolver #{} query for "{}" succeeded ({} bytes))", id_, host, response->body.size());

    return {body_data, body_data + response->body.size()};
}

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::unique_ptr<HttpClient> http_client, std::string server)
    : impl_(std::make_unique<Impl>(std::move(http_client), std::move(server), get_id())) {
}

DohResolver::~DohResolver() = default;

std::vector<std::uint8_t> DohResolver::query(const std::string &host, DNS::Type type) const {
    return impl_->query(host, type);
}
