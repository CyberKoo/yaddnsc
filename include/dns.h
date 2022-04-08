//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DNS_H
#define YADDNSC_DNS_H

#include <string>
#include <vector>
#include <optional>

#include "type.h"

class DNS {
public:
    struct dns_server_t {
        std::string ip_address;
        unsigned short port;
    };

    static std::vector<std::string> resolve(std::string_view, dns_record_t);

    static std::vector<std::string> resolve(std::string_view, dns_record_t, std::optional<dns_server_t>);

private:
    static int get_dns_type(dns_record_t);
};

#endif //YADDNSC_DNS_H
