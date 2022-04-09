
//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_IP_UTILS_H
#define YADDNSC_IP_UTILS_H

#include <string>
#include <vector>
#include <optional>

#include "type.h"

namespace IPUtil {
    std::vector<std::string> get_ip_from_interface(std::string_view, ip_version_t);

    std::optional<std::string> get_ip_from_url(std::string_view url, ip_version_t version, const char *nif_name);

    int ip2af(ip_version_t version);
}


#endif //YADDNSC_IP_UTILS_H
