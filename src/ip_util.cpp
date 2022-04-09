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
#include "httpclient.h"
#include "string_util.h"
#include "network_util.h"

std::vector<std::string> IPUtil::get_ip_from_interface(std::string_view nif_name, ip_version_t version) {
    auto addresses = NetworkUtil::get_nif_ip_address(nif_name);
    std::vector<std::string> nif_addresses;
    for (auto &[addr, addr_af]: addresses) {
        if (addr_af == ip2af(version)) {
            nif_addresses.emplace_back(addr);
        }
    }

    return nif_addresses;
}

std::optional<std::string> IPUtil::get_ip_from_url(std::string_view url, ip_version_t version, const char *nif_name) {
    SPDLOG_DEBUG("Trying get ip address from {}", url);

    auto parsed = Uri::parse(url);
    auto response = HttpClient::get(parsed, ip2af(version), nif_name);
    if (response) {
        auto body = response->body;
        StringUtil::trim(body);
        SPDLOG_DEBUG("HTTP response: {}", body);
        return body;
    } else {
        SPDLOG_WARN("Unable to get ip address from {}, Error: {}", url, httplib::to_string(response.error()));
        return std::nullopt;
    }
}

int IPUtil::ip2af(ip_version_t version) {
    switch (version) {
        case ip_version_t::IPV4:
            return AF_INET;
        case ip_version_t::IPV6:
            return AF_INET6;
        case ip_version_t::UNSPECIFIED:
            return AF_UNSPEC;
        default:
            return AF_UNSPEC;
    }
}
