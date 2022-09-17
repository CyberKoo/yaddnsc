//
// Created by Kotarou on 2022/4/6.
//

#include "config.h"

#include <fstream>
#include <optional>
#include <filesystem>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "type.h"

template<typename T>
std::optional<T> get_optional(const nlohmann::json &j, const std::string &key) try {
    return j.at(key).get<T>();
} catch (const nlohmann::json::exception &) {
    return std::nullopt;
}

void Config::from_json(const nlohmann::json &j, driver_config &driver) {
    driver.driver_dir = get_optional<std::string>(j, "driver_dir").value_or("");
    j.at("load").get_to(driver.load);
}

void Config::from_json(const nlohmann::json &j, config &config) {
    j.at("driver").get_to(config.driver);
    j.at("resolver").get_to(config.resolver);
    j.at("domains").get_to(config.domains);
}

void Config::from_json(const nlohmann::json &j, resolver_config &resolver) {
    j.at("use_custom_server").get_to(resolver.use_custom_server);
    j.at("ipaddress").get_to(resolver.ip_address);
    j.at("port").get_to(resolver.port);
}

void Config::from_json(const nlohmann::json &j, subdomain_config &subdomain) {
    j.at("name").get_to(subdomain.name);
    j.at("type").get_to(subdomain.type);
    j.at("interface").get_to(subdomain.interface);
    j.at("ip_source").get_to(subdomain.ip_source);
    j.at("ip_source_param").get_to(subdomain.ip_source_param);
    j.at("driver_param").get_to(subdomain.driver_param);
    subdomain.ip_type = get_optional<ip_version_type>(j, "ip_type").value_or(ip_version_type::UNSPECIFIED);
    subdomain.allow_ula = get_optional<bool>(j, "allow_ula").value_or(false);
    subdomain.allow_local_link = get_optional<bool>(j, "allow_local_link").value_or(false);
}

void Config::from_json(const nlohmann::json &j, domain_config &domain) {
    j.at("name").get_to(domain.name);
    j.at("update_interval").get_to(domain.update_interval);
    j.at("force_update").get_to(domain.force_update);
    j.at("driver").get_to(domain.driver);
    j.at("subdomains").get_to(domain.subdomains);
}

void Config::from_json(const nlohmann::json &j, ip_source_type &e) {
    static const std::pair<ip_source_type, nlohmann::json> m[] = {
            {ip_source_type::INTERFACE, "interface"},
            {ip_source_type::URL,       "url"}
    };

    auto it = std::find_if(std::begin(m), std::end(m),
                           [&j](const std::pair<ip_source_type, nlohmann::json> &ej_pair) -> bool {
                               return ej_pair.second == j;
                           });

    e = ((it != std::end(m)) ? it : std::begin(m))->first;
}

NLOHMANN_JSON_SERIALIZE_ENUM(dns_record_type, {
    { dns_record_type::A, "a" },
    { dns_record_type::AAAA, "aaaa" },
    { dns_record_type::TXT, "txt" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(ip_version_type, {
    { ip_version_type::IPV6, "ipv4" },
    { ip_version_type::IPV4, "ipv6" },
    { ip_version_type::UNSPECIFIED, "unspecified" },
})

Config::config Config::load_config(std::string_view config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("config {} not exists", config_path));
    }

    std::ifstream fin(config_path.data());
    if (!fin) {
        throw std::runtime_error("failed to read config file");
    }

    try {
        auto json = nlohmann::json::parse(fin);
        return json.get<Config::config>();
    } catch (nlohmann::detail::parse_error &e) {
        throw std::runtime_error(fmt::format("failed to parse config file, error {}", e.what()));
    }
}
