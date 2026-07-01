//
// Created by Kotarou on 2026/7/1.
//

#include "http.h"

#include <spdlog/spdlog.h>

#include "network/http_client.h"
#include "network/inet_address.h"
#include "string_util.h"

HttpIpSource::HttpIpSource(std::string url, address_family_type af, std::string bind_interface)
    : url_(std::move(url)), af_(af), bind_interface_(std::move(bind_interface)) {
}

std::vector<InetAddress> HttpIpSource::resolve() const {
    HttpClientOptions opts{
        .address_family = af_,
        .interface = bind_interface_.empty() ? std::nullopt : std::optional<std::string>(bind_interface_),
    };

    auto body = TransientHttpClient::get_body(url_, opts);
    if (!body) {
        SPDLOG_DEBUG(R"(Failed to fetch IP from "{}")", url_);
        return {};
    }

    StringUtil::trim(*body);
    SPDLOG_DEBUG("HTTP response from {}: {}", url_, *body);

    auto addr = InetAddress::parse(*body);
    if (!addr) {
        return {};
    }
    return {*std::move(addr)};
}
