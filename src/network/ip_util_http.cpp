//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <optional>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "type.h"
#include "http_client.h"
#include "string_util.h"

std::optional<std::string> IPUtil::get_ip_from_url(std::string_view url, address_family version, const char *if_name) {
    auto body = HttpClient::get_body(url, version, if_name);
    if (!body) {
        return std::nullopt;
    }

    StringUtil::trim(*body);
    SPDLOG_DEBUG("HTTP response: {}", *body);
    return body;
}
