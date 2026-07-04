//
// Created by Kotarou on 2026/7/1.
//

#include "http.h"

#include <spdlog/spdlog.h>

#include "string_util.hpp"
#include "network/http_client.h"
#include "network/inet_address.h"

HttpIpSource::HttpIpSource(std::string url, AddressFamily address_family, std::string bind_interface)
    : url_(std::move(url)), address_family_(address_family), bind_interface_(std::move(bind_interface)) {
}

std::vector<InetAddress> HttpIpSource::resolve() const {
    HttpClientOptions opts{
        .address_family = address_family_,
        .interface = bind_interface_.empty() ? std::nullopt : std::optional(bind_interface_),
    };

    const auto body = TransientHttpClient::get_body(url_, opts);
    if (!body) {
        SPDLOG_DEBUG(R"(Failed to fetch IP from "{}")", url_);
        return {};
    }

    auto addr = InetAddress::parse(StringUtil::trim(*body));
    if (!addr) {
        return {};
    }
    SPDLOG_DEBUG("Resolved IP from HTTP: {}", addr->to_string());
    return {*std::move(addr)};
}
