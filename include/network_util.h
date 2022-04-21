//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_NETWORK_UTIL_H
#define YADDNSC_NETWORK_UTIL_H

#include <map>
#include <memory>
#include <string>
#include <functional>

struct ifaddrs;

class NetworkUtil {
public:
    static std::vector <std::string> get_interfaces();

    static std::map<std::string, int> get_nif_ip_address(std::string_view);

private:
    struct interface_addrs_t {
        std::string address;
        int inet_type;
    };

    using ifaddrs_ptr_t = std::unique_ptr <ifaddrs, std::function<void(ifaddrs *)>>;

    static ifaddrs_ptr_t get_ifaddrs();

    static std::map <std::string, std::vector<interface_addrs_t>> get_all_ip_addresses();

    static size_t get_address_struct_size(int);
};

#endif //YADDNSC_NETWORK_UTIL_H
