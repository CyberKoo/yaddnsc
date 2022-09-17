//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_H
#define YADDNSC_CONFIG_H

#include <string_view>
#include "nlohmann/json_fwd.hpp"

enum class dns_record_type;
enum class ip_version_type;

namespace Config {
    enum class ip_source_type {
        INTERFACE, URL
    };

    struct driver_config {
        std::string driver_dir;
        std::vector<std::string> load;
    };

    struct resolver_config {
        bool use_custom_server;
        std::string ip_address;
        unsigned short port;
    };

    struct subdomain_config {
        std::string name;
        dns_record_type type;
        std::string interface;
        ip_version_type ip_type;
        ip_source_type ip_source;
        std::string ip_source_param;
        bool allow_ula;
        bool allow_local_link;
        std::map<std::string, std::string> driver_param;
    };

    struct domain_config {
        std::string name;
        int update_interval;
        int force_update;
        std::string driver;
        std::vector<subdomain_config> subdomains;
    };

    struct config {
        driver_config driver;
        resolver_config resolver;
        std::vector<domain_config> domains;
    };

    void from_json(const nlohmann::json &, driver_config &);

    void from_json(const nlohmann::json &, config &);

    void from_json(const nlohmann::json &, resolver_config &);

    void from_json(const nlohmann::json &, subdomain_config &);

    void from_json(const nlohmann::json &, domain_config &);

    void from_json(const nlohmann::json &, ip_source_type &);

    config load_config(std::string_view config_path);
}

#endif //YADDNSC_CONFIG_H
