
//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_IP_UTILS_H
#define YADDNSC_IP_UTILS_H

#include <string>
#include <vector>
#include <optional>

enum class ip_version_type;

namespace IPUtil {
    std::vector<std::string> get_ip_from_interface(std::string_view, ip_version_type);

    std::optional<std::string> get_ip_from_url(std::string_view, ip_version_type, const char *);

    int ip2af(ip_version_type);

    bool is_ipv4_address(std::string_view);

    bool is_ipv6_address(std::string_view);
}

#endif //YADDNSC_IP_UTILS_H
