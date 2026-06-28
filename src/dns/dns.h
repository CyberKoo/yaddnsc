//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DNS_DNS_H
#define YADDNSC_DNS_DNS_H

#include <memory>
#include <string>
#include <vector>

#include "type.h"

class ResolverBase;

namespace DNS {
    std::vector<std::string> resolve(const std::string &host, dns_type type,
                                     const std::vector<std::shared_ptr<ResolverBase>> &resolvers);

    std::string_view error_to_str(dns_error error);

    address_family dns2ip(dns_type type);

    // Convert dns_type to the corresponding <arpa/nameser.h> ns_t_* constant.
    [[nodiscard]] int to_ns_type(dns_type type) noexcept;
}

#endif //YADDNSC_DNS_DNS_H
