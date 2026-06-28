//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_CONFIG_H
#define YADDNSC_CONFIG_CONFIG_H

#include <optional>
#include <vector>
#include <string>

#include <glaze/glaze.hpp>

#include "type.h"

namespace Config {
    enum class ip_source_type {
        INTERFACE, URL
    };

    enum class resolver_strategy {
        FALLBACK, CONCURRENT
    };

    struct driver_config {
        std::string driver_dir;
        bool auto_discover{false};
        std::vector<std::string> load;
    };

    struct resolver_config {
        bool use_custom_server{false};
        std::string address;
        unsigned short port{53};
        std::vector<dns_server_type> servers;
        resolver_strategy strategy{resolver_strategy::CONCURRENT};
    };

    struct subdomain_config {
        std::string name;
        dns_type type{};
        std::optional<std::string> interface;
        address_family_type ip_type{address_family_type::UNSPECIFIED};
        ip_source_type ip_source{};
        std::string ip_source_param;
        bool allow_ula{false};
        bool allow_local_link{false};
        int update_interval{}; // 0 = inherit from domain
        glz::generic driver_param;
    };

    struct domain_config {
        std::string name;
        int update_interval{};
        int force_update{};
        std::string driver;
        std::vector<subdomain_config> subdomains;
    };

    struct config {
        driver_config driver;
        resolver_config resolver;
        std::vector<domain_config> domains;
    };

    config load_config(const std::string &config_path);
}

#endif //YADDNSC_CONFIG_CONFIG_H
