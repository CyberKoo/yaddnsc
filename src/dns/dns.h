//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DNS_DNS_H
#define YADDNSC_DNS_DNS_H

#include <string>
#include <vector>

#include "type.h"

namespace DNS {
    std::vector<std::string> resolve(const std::string &host, dns_type type, const std::vector<DnsServer> &servers);

    std::string_view error_to_str(dns_error error);

    address_family dns2ip(dns_type type);
}

#endif //YADDNSC_DNS_DNS_H
