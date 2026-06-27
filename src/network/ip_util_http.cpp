//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <sys/socket.h>

#include <optional>

#include <spdlog/spdlog.h>

#include "http_client.h"
#include "string_util.h"
#include "type.h"

std::optional<std::string> IPUtil::get_ip_from_url(std::string_view url, address_family version,
                                                   const std::optional<std::string> &if_name) {
    auto body = HttpClient::get_body(url, version, if_name);
    if (!body) {
        return std::nullopt;
    }

    StringUtil::trim(*body);
    SPDLOG_DEBUG("HTTP response: {}", *body);
    return body;
}
