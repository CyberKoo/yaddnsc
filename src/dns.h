//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DNS_H
#define YADDNSC_DNS_H

#include <string>
#include <vector>
#include <optional>

#include "type.h"

namespace DNS {
    std::vector<std::string> resolve(const std::string &, dns_type, const std::optional<dns_server> &);

    std::string_view error_to_str(dns_error);

    std::string_view to_string(dns_type type);

    address_family dns2ip(dns_type type);
}

#endif //YADDNSC_DNS_H
