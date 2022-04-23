//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_UTIL_H
#define YADDNSC_NETWORK_UTIL_H

#include <map>
#include <string>
#include <vector>

namespace NetworkUtil {
    std::vector<std::string> get_interfaces();

    std::map<std::string, int> get_nif_ip_address(std::string_view);
}

#endif //YADDNSC_NETWORK_UTIL_H
