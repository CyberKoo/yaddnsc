//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_H
#define YADDNSC_CONFIG_H

#include <string_view>
#include <vector>
#include <map>

#include <glaze/glaze.hpp>

#include "type.h"

namespace Config {
    enum class ip_source_type {
        INTERFACE, URL
    };

    struct driver_config {
        std::string driver_dir;
        std::vector<std::string> load;
    };

    struct resolver_config {
        bool use_custom_server{};
        std::string ip_address;
        unsigned short port{};
    };

    struct subdomain_config {
        std::string name;
        dns_record_type type{};
        std::string interface;
        ip_version_type ip_type = ip_version_type::UNSPECIFIED;
        ip_source_type ip_source{};
        std::string ip_source_param;
        bool allow_ula = false;
        bool allow_local_link = false;
        std::map<std::string, std::string> driver_param;
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

    config load_config(std::string_view config_path);
}

template <>
struct glz::meta<Config::driver_config> {
    using T = Config::driver_config;
    static constexpr auto value = glz::object(
        "driver_dir", &T::driver_dir,
        "load", &T::load
    );
};

template <>
struct glz::meta<Config::resolver_config> {
    using T = Config::resolver_config;
    static constexpr auto value = glz::object(
        "use_custom_server", &T::use_custom_server,
        "ipaddress", &T::ip_address,
        "port", &T::port
    );
};

template <>
struct glz::meta<Config::subdomain_config> {
    using T = Config::subdomain_config;
    static constexpr auto value = glz::object(
        "name", &T::name,
        "type", &T::type,
        "interface", &T::interface,
        "ip_type", &T::ip_type,
        "ip_source", &T::ip_source,
        "ip_source_param", &T::ip_source_param,
        "allow_ula", &T::allow_ula,
        "allow_local_link", &T::allow_local_link,
        "driver_param", &T::driver_param
    );
};

template <>
struct glz::meta<Config::domain_config> {
    using T = Config::domain_config;
    static constexpr auto value = glz::object(
        "name", &T::name,
        "update_interval", &T::update_interval,
        "force_update", &T::force_update,
        "driver", &T::driver,
        "subdomains", &T::subdomains
    );
};

template <>
struct glz::meta<Config::config> {
    using T = Config::config;
    static constexpr auto value = glz::object(
        "driver", &T::driver,
        "resolver", &T::resolver,
        "domains", &T::domains
    );
};

template <>
struct glz::meta<Config::ip_source_type> {
    using enum Config::ip_source_type;
    static constexpr auto value = glz::enumerate(
        "interface", INTERFACE,
        "url", URL
    );
};

template <>
struct glz::meta<dns_record_type> {
    using enum dns_record_type;
    static constexpr auto value = glz::enumerate(
        "a", A,
        "aaaa", AAAA,
        "txt", TXT,
        "soa", SOA
    );
};

template <>
struct glz::meta<ip_version_type> {
    using enum ip_version_type;
    static constexpr auto value = glz::enumerate(
        "ipv6", IPV6,
        "ipv4", IPV4,
        "unspecified", UNSPECIFIED
    );
};

#endif //YADDNSC_CONFIG_H
