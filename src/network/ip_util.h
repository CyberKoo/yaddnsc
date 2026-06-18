
//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_IP_UTILS_H
#define YADDNSC_NETWORK_IP_UTILS_H

#include <map>
#include <string>
#include <vector>
#include <optional>

enum class address_family;

namespace IPUtil {
    std::vector<std::string> extract_address(const std::map<std::string, int> &, address_family);

    std::optional<std::string> get_ip_from_url(std::string_view, address_family, const char *);

    int to_socket_type(address_family);

    bool is_ipv4_address(const std::string &);

    bool is_ipv6_address(const std::string &);

    // IPv6 address classification
    bool is_ipv6_local_link(const std::string &);

    bool is_ipv6_site_local(const std::string &);

    bool is_ipv6_ula(const std::string &);
};

#endif //YADDNSC_NETWORK_IP_UTILS_H
