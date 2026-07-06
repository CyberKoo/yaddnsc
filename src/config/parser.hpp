//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_CONFIG_PARSER_HPP
#define YADDNSC_CONFIG_PARSER_HPP

#include <glaze/glaze.hpp>

#include "config.h"

/// glz::meta specialisation for Config::DriverConfig JSON mapping.
template<>
struct glz::meta<Config::DriverConfig> {
    using T = Config::DriverConfig;
    static constexpr auto value = object(
        "driver_dir", &T::driver_dir,
        "auto_discover", &T::auto_discover,
        "load", &T::load
    );
};

/// glz::meta specialisation for DNS::Server JSON mapping.
/// Supports both "address" and "ipaddress" keys for backward compatibility.
template<>
struct glz::meta<DNS::Server> {
    using T = DNS::Server;
    static constexpr auto value = object(
        "address", &T::address,
        "ipaddress", &T::address,
        "port", &T::port
    );
};

/// glz::meta specialisation for Config::ResolverConfig JSON mapping.
template<>
struct glz::meta<Config::ResolverConfig> {
    using T = Config::ResolverConfig;
    static constexpr auto value = object(
        "use_custom_server", &T::use_custom_server,
        "address", &T::address,
        "ipaddress", &T::address,
        "port", &T::port,
        "servers", &T::servers,
        "strategy", &T::strategy
    );
};

/// glz::meta specialisation for Config::ResolverStrategy enum JSON mapping.
template<>
struct glz::meta<Config::ResolverStrategy> {
    using enum Config::ResolverStrategy;
    static constexpr auto value = enumerate(
        "fallback", FALLBACK,
        "concurrent", CONCURRENT
    );
};

/// glz::meta specialisation for Config::SubdomainConfig JSON mapping.
template<>
struct glz::meta<Config::SubdomainConfig> {
    using T = Config::SubdomainConfig;
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

/// glz::meta specialisation for Config::DomainConfig JSON mapping.
template<>
struct glz::meta<Config::DomainConfig> {
    using T = Config::DomainConfig;
    static constexpr auto value = object(
        "name", &T::name,
        "update_interval", &T::update_interval,
        "force_update", &T::force_update,
        "driver", &T::driver,
        "subdomains", &T::subdomains
    );
};

/// glz::meta specialisation for Config::AppConfig (top-level) JSON mapping.
template<>
struct glz::meta<Config::AppConfig> {
    using T = Config::AppConfig;
    static constexpr auto value = object(
        "driver", &T::driver,
        "resolver", &T::resolver,
        "domains", &T::domains
    );
};

/// glz::meta specialisation for Config::IpSource enum JSON mapping.
/// Supports both "interface", "http" / "url", and "mdns".
template<>
struct glz::meta<Config::IpSource> {
    using enum Config::IpSource;
    static constexpr auto value = enumerate(
        "interface", INTERFACE,
        "http", HTTP,
        "url", HTTP, // backward compatibility
        "mdns", MDNS
    );
};

/// glz::meta specialisation for DNS::Type enum JSON mapping.
template<>
struct glz::meta<DNS::Type> {
    using enum DNS::Type;
    static constexpr auto value = enumerate(
        "a", A,
        "aaaa", AAAA,
        "txt", TXT,
        "soa", SOA
    );
};

/// glz::meta specialisation for AddressFamily enum JSON mapping.
template<>
struct glz::meta<AddressFamily> {
    using enum AddressFamily;
    static constexpr auto value = enumerate(
        "ipv6", IPV6,
        "ipv4", IPV4,
        "unspecified", UNSPECIFIED
    );
};

#endif //YADDNSC_CONFIG_PARSER_HPP
