//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DNS_H
#define YADDNSC_DNS_H

#include <string>
#include <vector>
#include <memory>
#include <optional>

#include "type.h"

namespace DNS {
    std::vector<std::string> resolve(std::string_view, dns_record_t, std::optional<dns_server_t> = std::nullopt);

    std::string_view error_to_str(dns_lookup_error_t);
}

#endif //YADDNSC_DNS_H
