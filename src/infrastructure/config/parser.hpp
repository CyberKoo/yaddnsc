#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_PARSER_HPP
#define YADDNSC_INFRASTRUCTURE_CONFIG_PARSER_HPP

#include <glaze/glaze.hpp>

// IWYU misses that the glz::meta specialisations below name these types.

#include "config.h"                          // IWYU pragma: keep
#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/dns/record_kind.h"

/// glz::meta specialisation for Config::DriverConfig JSON mapping.
template<>
struct glz::meta<Config::DriverConfig> {
    using T = Config::DriverConfig;
    static constexpr auto value =
        object("driver_dir", &T::driver_dir, "auto_discover", &T::auto_discover, "load", &T::load);
};

/// glz::meta specialisation for DnsServer JSON mapping.
/// Supports both "address" and "ipaddress" keys for backward compatibility.
template<>
struct glz::meta<domain::DnsServer> {
    using T = domain::DnsServer;
    static constexpr auto value = object("address", &T::address, "ipaddress", &T::address, "port", &T::port);
};

/// glz::meta specialisation for Config::ResolverConfig JSON mapping.
template<>
struct glz::meta<Config::ResolverConfig> {
    using T = Config::ResolverConfig;
    static constexpr auto value =
        object("use_custom_servers", &T::use_custom_servers, "servers", &T::servers, "strategy", &T::strategy);
};

/// glz::meta specialisation for domain::ResolverStrategy enum JSON mapping.
template<>
struct glz::meta<domain::ResolverStrategy> {
    using enum domain::ResolverStrategy;
    static constexpr auto value = enumerate("fallback", FALLBACK, "concurrent", CONCURRENT, "shuffle", SHUFFLE);
};

/// glz::meta specialisation for Config::SubdomainConfig JSON mapping.
template<>
struct glz::meta<Config::SubdomainConfig> {
    using T = Config::SubdomainConfig;
    static constexpr auto value =
        object("name", &T::name, "type", &T::type, "interface", &T::interface, "ip_source", &T::ip_source,
               "ip_source_param", &T::ip_source_param, "allow_ula", &T::allow_ula, "allow_local_link",
               &T::allow_local_link, "update_interval", &T::update_interval, "driver_params", &T::driver_params);
};

/// glz::meta specialisation for Config::DomainConfig JSON mapping.
template<>
struct glz::meta<Config::DomainConfig> {
    using T = Config::DomainConfig;
    static constexpr auto value = object("name", &T::name, "update_interval", &T::update_interval, "force_update",
                                         &T::force_update, "driver", &T::driver, "subdomains", &T::subdomains);
};

/// glz::meta specialisation for Config::AppConfig (top-level) JSON mapping.
template<>
struct glz::meta<Config::AppConfig> {
    using T = Config::AppConfig;
    static constexpr auto value = object("drivers", &T::drivers, "resolver", &T::resolver, "domains", &T::domains,
                                         "bootstrap_dns", &T::bootstrap_dns);
};

/// glz::meta specialisation for domain::IpSource enum JSON mapping.
/// Supports both "interface", "http" / "url", and "mdns".
template<>
struct glz::meta<domain::IpSource> {
    using enum domain::IpSource;
    static constexpr auto value = enumerate("interface", INTERFACE, "http", HTTP, "url",
                                            HTTP,  // backward compatibility
                                            "mdns", MDNS);
};

/// glz::meta specialisation for RecordKind enum JSON mapping.
template<>
struct glz::meta<domain::RecordKind> {
    using enum domain::RecordKind;
    static constexpr auto value = enumerate("a", A, "aaaa", AAAA, "txt", TXT);
};

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_PARSER_HPP
