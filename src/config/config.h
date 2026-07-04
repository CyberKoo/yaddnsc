//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_CONFIG_H
#define YADDNSC_CONFIG_CONFIG_H

#include <optional>
#include <vector>
#include <string>

#include <glaze/glaze.hpp>

#include "dns_type.h"
#include "address_family.h"

namespace Config {
    enum class IpSource {
        INTERFACE, HTTP, MDNS
    };

    enum class ResolverStrategy {
        FALLBACK, CONCURRENT
    };

    struct DriverConfig {
        std::optional<std::string> driver_dir;
        bool auto_discover{false};
        std::vector<std::string> load;
    };

    struct ResolverConfig {
        bool use_custom_server{false};
        std::string address;
        unsigned short port{53};
        std::vector<DNS::Server> servers;
        ResolverStrategy strategy{ResolverStrategy::CONCURRENT};
    };

    struct SubdomainConfig {
        std::string name;
        DNS::Type type{};
        std::string interface;
        AddressFamily ip_type{AddressFamily::UNSPECIFIED};
        IpSource ip_source{};
        std::string ip_source_param;
        bool allow_ula{false};
        bool allow_local_link{false};
        int update_interval{}; // 0 = inherit from domain
        glz::generic driver_param;
    };

    struct DomainConfig {
        std::string name;
        int update_interval{};
        int force_update{};
        std::string driver;
        std::vector<SubdomainConfig> subdomains;
    };

    struct AppConfig {
        DriverConfig driver;
        ResolverConfig resolver;
        std::vector<DomainConfig> domains;
    };

    AppConfig load_config(const std::string &config_path);
}

#endif //YADDNSC_CONFIG_CONFIG_H
