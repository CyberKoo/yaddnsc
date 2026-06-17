//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <optional>
#include <sys/socket.h>

#include <httplib.h>
#include <spdlog/spdlog.h>

#include "uri.h"
#include "type.h"
#include "http_client.h"
#include "string_util.h"

std::optional<std::string> IPUtil::get_ip_from_url(std::string_view url, address_family version, const char *if_name) {
    const auto parsed = Uri::parse(url);
    auto response = HttpClient::get(parsed, ip2af(version), if_name);
    if (response) {
        auto body = response->body;
        StringUtil::trim(body);
        SPDLOG_DEBUG("HTTP response: {}", body);
        return body;
    }

    SPDLOG_WARN(R"(Failed to obtain IP address from "{}", error: {})", url, httplib::to_string(response.error()));
    return std::nullopt;
}
