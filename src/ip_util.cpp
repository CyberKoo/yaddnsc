//
// Created by Kotarou on 2022/4/5.
//

#include "ip_util.h"

#include <vector>
#include <optional>
#include <sys/socket.h>

#include <httplib.h>
#include <spdlog/spdlog.h>

#include "uri.h"
#include "type.h"
#include "httpclient.h"
#include "string_util.h"
#include "network_util.h"

std::vector<std::string> IPUtil::get_ip_from_interface(std::string_view nif_name, ip_version_type version) {
    auto addresses = NetworkUtil::get_nif_ip_address(nif_name);
    std::vector<std::string> nif_addresses;
    for (auto &[ip_address, family]: addresses) {
        if (version == ip_version_type::UNSPECIFIED || family == ip2af(version)) {
            nif_addresses.emplace_back(ip_address);
        }
    }

    return nif_addresses;
}

std::optional<std::string> IPUtil::get_ip_from_url(std::string_view url, ip_version_type version, const char *if_name) {
    auto parsed = Uri::parse(url);
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

int IPUtil::ip2af(ip_version_type version) {
    switch (version) {
        case ip_version_type::IPV4:
            return AF_INET;
        case ip_version_type::IPV6:
            return AF_INET6;
        case ip_version_type::UNSPECIFIED:
            return AF_UNSPEC;
        default:
            return AF_UNSPEC;
    }
}

bool IPUtil::is_ipv4_address(std::string_view str) {
    sockaddr_in sa{};
    return inet_pton(AF_INET, str.data(), &(sa.sin_addr)) != 0;
}

bool IPUtil::is_ipv6_address(std::string_view str) {
    sockaddr_in6 sa{};
    return inet_pton(AF_INET6, str.data(), &(sa.sin6_addr)) != 0;
}
