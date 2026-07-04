//
// Created by Kotarou on 2026/6/28.
//

#include "doh_resolver.h"

#include <arpa/nameser.h>
#include <httplib.h>
#include <spdlog/spdlog.h>

#include "dns_mkquery.h"
#include "exception/dns_lookup_exception.h"
#include "cert_util.h"
#include "uri.h"
#include "version.h"

namespace {
    constexpr auto DOH_CONTENT_TYPE = "application/dns-message";
} // anonymous namespace

class DohResolver::Impl {
public:
    explicit Impl(std::string server_url)
        : server_url_(std::move(server_url)) {
    }

    std::vector<uint8_t> query(const std::string &host, int ns_type) {
        if (ns_type == ns_t_invalid) {
            throw DnsLookupException("Unsupported dns_type for DoH query", dns_lookup_error_type::UNKNOWN);
        }

        SPDLOG_DEBUG(R"(DoH lookup for domain "{}" (type {}))", host, ns_type);

        const auto query_bytes = dns_mkquery(host, ns_type);

        // Parse the server URL to get host, port, path
        const auto uri = Uri::parse(server_url_);
        auto &client = get_http_client(uri);

        httplib::Headers headers = {
            {"Accept", DOH_CONTENT_TYPE}
        };

        std::string path(uri.get_path());
        if (path.empty()) {
            path = "/dns-query";
        }

        SPDLOG_TRACE(R"(DoH POST {} ({} bytes))", server_url_, query_bytes.size());

        auto body = std::string(reinterpret_cast<const char *>(query_bytes.data()), query_bytes.size());
        auto response = client.Post(path, headers, body, DOH_CONTENT_TYPE);

        if (!response) {
            throw DnsLookupException(
                fmt::format(R"(DoH query to "{}" failed: {})", server_url_,
                            httplib::to_string(response.error())),
                dns_lookup_error_type::CONNECTION);
        }

        if (response->status != 200) {
            const auto is_transient = response->status >= 500;
            throw DnsLookupException(
                fmt::format(R"(DoH server "{}" returned HTTP status {})", server_url_, response->status),
                is_transient ? dns_lookup_error_type::UNKNOWN : dns_lookup_error_type::NODATA);
        }

        const auto &resp_body = response->body;
        SPDLOG_DEBUG(R"(DoH query to "{}" succeeded ({} bytes) for "{}")", server_url_, resp_body.size(), host);

        return {resp_body.begin(), resp_body.end()};
    }

private:
    httplib::Client &get_http_client(const Uri &uri) {
        if (!http_client_) {
            SPDLOG_DEBUG(R"(DoH: connecting to "{}:{}{}")", uri.get_host(), uri.get_port(), uri.get_path());

            auto client = std::make_unique<httplib::Client>(
                fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port()));
            client->set_connection_timeout(std::chrono::seconds(10));
            client->set_read_timeout(std::chrono::seconds(10));
            client->set_follow_location(true);
            client->set_default_headers({{"User-Agent", get_full_version()}});

            auto ca_path = get_system_ca_path();
            if (!ca_path.empty()) {
                client->set_ca_cert_path(ca_path.data());
                client->enable_server_certificate_verification(true);
            }

            http_client_ = std::move(client);
        }
        return *http_client_;
    }

    const std::string server_url_;
    mutable std::unique_ptr<httplib::Client> http_client_;
};

DohResolver::DohResolver(std::string server_url)
    : impl_(std::make_unique<Impl>(std::move(server_url))) {
}

DohResolver::~DohResolver() = default;

std::vector<uint8_t> DohResolver::query(const std::string &host, int ns_type) {
    return impl_->query(host, ns_type);
}
