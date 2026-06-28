//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"

#include <resolv.h>
#include <arpa/nameser.h>

#include <mutex>

#include <spdlog/spdlog.h>

#include "types.h"
#include "fmt.hpp"
#include "http_types.h"
#include "network/http_client.h"
#include "exception/dns_lookup_exception.h"

namespace {
    constexpr auto DOH_CONTENT_TYPE = "application/dns-message";
} // anonymous namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

class DohResolver::Impl {
public:
    explicit Impl(std::unique_ptr<HttpClient> http_client, std::string server) : server_(std::move(server)),
        http_client_(std::move(http_client)) {
    }

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, dns_type type) const {
        const auto ns_type = DNS::to_ns_type(type);
        if (ns_type == ns_t_invalid) {
            throw DnsLookupException(
                fmt::format(R"(Unsupported dns_type for DoH query: "{}")", host),
                dns_error_type::UNKNOWN
            );
        }

        SPDLOG_DEBUG(R"(DoH lookup for domain "{}" (type {}))", host, ns_type);

        const auto query_bytes = build_query(host, ns_type);
        const std::string body_str(query_bytes.begin(), query_bytes.end());

        http_request req;
        req.url = server_;
        req.request_method = http_method_type::POST;
        req.content_type = DOH_CONTENT_TYPE;
        req.header = {{"Accept", DOH_CONTENT_TYPE}};
        req.body = body_str;

        SPDLOG_TRACE(R"(DoH POST {}  ({} bytes))", server_, body_str.size());

        const auto response = http_client_->send(req);

        if (!response) {
            throw DnsLookupException(
                fmt::format(R"(DoH query to "{}" failed: {})", req.url, response.error()),
                dns_error_type::CONNECTION
            );
        }

        if (response->status_code != 200) {
            // 4xx: client errors are definitive (no point retrying)
            // 5xx: server errors are transient (may succeed on another server or retry)
            const auto is_transient = response->status_code >= 500;
            throw DnsLookupException(
                fmt::format(R"(DoH server "{}" returned HTTP status {})", req.url, response->status_code),
                is_transient ? dns_error_type::UNKNOWN : dns_error_type::NODATA
            );
        }

        SPDLOG_DEBUG(R"(DoH query to "{}" succeeded ({} bytes) for "{}")", req.url, response->body.size(), host);

        return {response->body.begin(), response->body.end()};
    }

private:
    // Build a raw DNS query packet via res_mkquery.
    //
    // Delegates packet construction to libresolv's res_mkquery, which handles
    // label encoding, random transaction IDs, and EDNS0 if the system resolver
    // configuration requests it.
    //
    // res_mkquery is chosen over res_nmkquery because it is available on all
    // platforms (glibc, musl, macOS) where the per-thread reentrant variant is
    // not — matching the portability considerations in DnsResolver.
    static std::vector<uint8_t> build_query(const std::string &host, int ns_type) {
        // res_mkquery may append EDNS0 records from the system resolver config
        // (e.g. /etc/resolv.conf), so allocate a generous buffer.
        static constexpr size_t BUFFER_SIZE = 4096;
        std::vector<uint8_t> buf(BUFFER_SIZE);

        // res_mkquery reads/writes the process-global _res state (e.g. _res.id),
        // so it must be serialized — same pattern as DnsResolver's non-reentrant
        // fallback in dns_resolver.cpp.
        static std::mutex res_mutex;
        std::lock_guard lock(res_mutex);

        const int len = res_mkquery(ns_o_query, host.c_str(), ns_c_in, ns_type, nullptr, 0, nullptr, buf.data(),
                                    static_cast<int>(buf.size()));

        if (len < 0) {
            throw DnsLookupException(
                fmt::format(R"(Failed to construct DNS query packet for "{}")", host),
                dns_error_type::UNKNOWN
            );
        }

        buf.resize(static_cast<size_t>(len));
        return buf;
    }

    const std::string server_;
    std::unique_ptr<HttpClient> http_client_;
};

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::unique_ptr<HttpClient> http_client, std::string server)
    : impl_(std::make_unique<Impl>(std::move(http_client), std::move(server))) {
}

DohResolver::~DohResolver() = default;

std::vector<uint8_t> DohResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}
