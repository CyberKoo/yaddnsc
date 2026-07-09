//
// Created by Kotarou on 2026/7/1.
//

#include "http.h"

#include "network/http_client.h"
#include "network/inet_address.h"

#include "uri.h"

#include "string_util.hpp"
#include <spdlog/spdlog.h>

// ===========================================================================
// HttpIpSource — fetch public IP from an external HTTP service.
// ===========================================================================

HttpIpSource::~HttpIpSource() = default;

HttpIpSource::HttpIpSource(std::string url, AddressFamily address_family, std::string bind_interface)
    : url_(std::move(url)), address_family_(address_family), bind_interface_(std::move(bind_interface)) {
    HttpClientOptions opts{
        .address_family = address_family_,
        .interface = bind_interface_.empty() ? std::nullopt : std::optional(bind_interface_),
    };

    auto uri = Uri::parse(url_);
    client_ = std::make_unique<PersistentHttpClient>(uri, std::move(opts));
}

// ---------------------------------------------------------------------------
// HttpIpSource::resolve — send GET request and parse the response body as an IP.
// ---------------------------------------------------------------------------

std::vector<InetAddress> HttpIpSource::resolve() const {
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto resp = client_->exchange(url_, req);
    if (!resp) {
        SPDLOG_DEBUG(R"(Failed to fetch IP from "{}")", url_);
        return {};
    }

    auto addr = InetAddress::parse(StringUtil::trim(resp->body));
    if (!addr) {
        return {};
    }
    SPDLOG_DEBUG("Resolved IP from HTTP: {}", addr->to_string());
    return {*std::move(addr)};
}
