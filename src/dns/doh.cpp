//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"
#include "dns_mkquery.h"

#include <arpa/nameser.h>
#include <resolv.h>

#include <spdlog/spdlog.h>

#include "exception/dns_lookup_exception.h"
#include "fmt.hpp"
#include "http_types.h"
#include "network/http_client.h"
#include "types.h"

namespace {
constexpr auto DOH_CONTENT_TYPE = "application/dns-message";
} // anonymous namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

class DohResolver::Impl {
public:
  explicit Impl(std::unique_ptr<HttpClient> http_client, std::string server)
      : server_(std::move(server)), http_client_(std::move(http_client)) {}

  [[nodiscard]] std::vector<uint8_t> query(const std::string &host,
                                           dns_type type) const {
    const auto ns_type = DNS::to_ns_type(type);
    if (ns_type == ns_t_invalid) {
      throw DnsLookupException(
          fmt::format(R"(Unsupported dns_type for DoH query: "{}")", host),
          dns_error_type::UNKNOWN);
    }

    SPDLOG_DEBUG(R"(DoH lookup for domain "{}" (type {}))", host, ns_type);

    const auto query_bytes = dns_mkquery(host, ns_type);

    http_request req{.url = server_,
                     .content_type = DOH_CONTENT_TYPE,
                     .request_method = http_method_type::POST,
                     .header = {{"Accept", DOH_CONTENT_TYPE}},
                     .body = std::string(query_bytes.begin(), query_bytes.end())};

    SPDLOG_TRACE(R"(DoH POST {}  ({} bytes))", server_, query_bytes.size());

    const auto response = http_client_->send(req);

    if (!response) {
      throw DnsLookupException(fmt::format(R"(DoH query to "{}" failed: {})",
                                           req.url, response.error()),
                               dns_error_type::CONNECTION);
    }

    if (response->status_code != 200) {
      // 4xx: client errors are definitive (no point retrying)
      // 5xx: server errors are transient (may succeed on another server or
      // retry)
      const auto is_transient = response->status_code >= 500;
      throw DnsLookupException(
          fmt::format(R"(DoH server "{}" returned HTTP status {})", req.url,
                      response->status_code),
          is_transient ? dns_error_type::UNKNOWN : dns_error_type::NODATA);
    }

    SPDLOG_DEBUG(R"(DoH query to "{}" succeeded ({} bytes) for "{}")", req.url,
                 response->body.size(), host);

    return {response->body.begin(), response->body.end()};
  }

private:
  const std::string server_;
  std::unique_ptr<HttpClient> http_client_;
};

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::unique_ptr<HttpClient> http_client,
                         std::string server)
    : impl_(std::make_unique<Impl>(std::move(http_client), std::move(server))) {
}

DohResolver::~DohResolver() = default;

std::vector<uint8_t> DohResolver::query(const std::string &host,
                                        dns_type type) const {
  return impl_->query(host, type);
}

std::string_view DohResolver::get_type() const noexcept {
  return "DNS-Over-HTTPS resolver";
}
