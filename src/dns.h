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
    std::vector<std::string> resolve(std::string_view, dns_record_type, const std::optional<dns_server> &);

    std::string_view error_to_str(dns_lookup_error_type);
}

#endif //YADDNSC_DNS_H
