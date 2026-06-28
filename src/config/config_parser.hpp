//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_CONFIG_CONFIG_PARSER_HPP
#define YADDNSC_CONFIG_CONFIG_PARSER_HPP

#include <glaze/glaze.hpp>

#include "config.h"

template<>
struct glz::meta<Config::driver_config> {
    using T = Config::driver_config;
    static constexpr auto value = object(
        "driver_dir", &T::driver_dir,
        "auto_discover", &T::auto_discover,
        "load", &T::load
    );
};

template<>
struct glz::meta<dns_server> {
    using T = dns_server;
    static constexpr auto value = object(
        "address", &T::address,
        "ipaddress", &T::address,
        "port", &T::port
    );
};

template<>
struct glz::meta<Config::resolver_config> {
    using T = Config::resolver_config;
    static constexpr auto value = object(
        "use_custom_server", &T::use_custom_server,
        "address", &T::address,
        "ipaddress", &T::address,
        "port", &T::port,
        "servers", &T::servers,
        "strategy", &T::strategy
    );
};

template<>
struct glz::meta<Config::resolver_strategy> {
    using enum Config::resolver_strategy;
    static constexpr auto value = enumerate(
        "fallback", FALLBACK,
        "concurrent", CONCURRENT
    );
};

template<>
struct glz::meta<Config::subdomain_config> {
    using T = Config::subdomain_config;
    static constexpr auto value = object(
        "name", &T::name,
        "type", &T::type,
        "interface", &T::interface,
        "ip_type", &T::ip_type,
        "ip_source", &T::ip_source,
        "ip_source_param", &T::ip_source_param,
        "allow_ula", &T::allow_ula,
        "allow_local_link", &T::allow_local_link,
        "update_interval", &T::update_interval,
        "driver_param", &T::driver_param
    );
};

template<>
struct glz::meta<Config::domain_config> {
    using T = Config::domain_config;
    static constexpr auto value = object(
        "name", &T::name,
        "update_interval", &T::update_interval,
        "force_update", &T::force_update,
        "driver", &T::driver,
        "subdomains", &T::subdomains
    );
};

template<>
struct glz::meta<Config::config> {
    using T = Config::config;
    static constexpr auto value = object(
        "driver", &T::driver,
        "resolver", &T::resolver,
        "domains", &T::domains
    );
};

template<>
struct glz::meta<Config::ip_source_type> {
    using enum Config::ip_source_type;
    static constexpr auto value = enumerate(
        "interface", INTERFACE,
        "url", URL
    );
};

template<>
struct glz::meta<dns_type> {
    using enum dns_type;
    static constexpr auto value = enumerate(
        "a", A,
        "aaaa", AAAA,
        "txt", TXT,
        "soa", SOA
    );
};

template<>
struct glz::meta<address_family> {
    using enum address_family;
    static constexpr auto value = enumerate(
        "ipv6", IPV6,
        "ipv4", IPV4,
        "unspecified", UNSPECIFIED
    );
};

#endif //YADDNSC_CONFIG_CONFIG_PARSER_HPP
