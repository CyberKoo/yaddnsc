//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_H
#define YADDNSC_CONFIG_H

#include <string_view>
#include "nlohmann/json_fwd.hpp"

enum class dns_record_t;
enum class ip_version_t;

namespace Config {
    enum class ip_source_t {
        INTERFACE, URL
    };

    struct driver_config_t {
        std::vector<std::string> load;
    };

    struct resolver_config_t {
        bool use_customise_server;
        std::string ip_address;
        unsigned short port;
    };

    struct sub_domain_config_t {
        std::string name;
        dns_record_t type;
        std::string interface;
        ip_version_t ip_type;
        ip_source_t ip_source;
        std::string ip_source_param;
        std::map<std::string, std::string> driver_param;
    };

    struct domains_config_t {
        std::string name;
        int update_interval;
        int force_update;
        std::string driver;
        std::vector<sub_domain_config_t> subdomains;
    };

    struct config_t {
        driver_config_t driver;
        resolver_config_t resolver;
        std::vector<domains_config_t> domains;
    };

    void from_json(const nlohmann::json &, driver_config_t &);

    void from_json(const nlohmann::json &, config_t &);

    void from_json(const nlohmann::json &, resolver_config_t &);

    void from_json(const nlohmann::json &, sub_domain_config_t &);

    void from_json(const nlohmann::json &, domains_config_t &);

    void from_json(const nlohmann::json &, ip_source_t &);

    config_t load_config(std::string_view config_path);
}

#endif //YADDNSC_CONFIG_H
