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

    static std::vector<std::string> resolve(std::string_view, dns_record_t, std::optional<dns_server_t> = std::nullopt);

    static std::string_view error_to_str(dns_lookup_error_t error);
private:
    constexpr static int MAXIMUM_UDP_SIZE = 512;

    static dns_lookup_error_t get_dns_lookup_err(int);

    static int get_dns_type(dns_record_t);
};

#endif //YADDNSC_DNS_H