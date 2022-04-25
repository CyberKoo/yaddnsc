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

void Config::from_json(const nlohmann::json &j, driver_config_t &driver) {
    j.at("load").get_to(driver.load);
}

void Config::from_json(const nlohmann::json &j, config_t &config) {
    j.at("driver").get_to(config.driver);
    j.at("resolver").get_to(config.resolver);
    j.at("domains").get_to(config.domains);
}

void Config::from_json(const nlohmann::json &j, resolver_config_t &resolver) {
    j.at("use_customise_server").get_to(resolver.use_customise_server);
    j.at("ipaddress").get_to(resolver.ip_address);
    j.at("port").get_to(resolver.port);
}

void Config::from_json(const nlohmann::json &j, sub_domain_config_t &sub_domain) {
    j.at("name").get_to(sub_domain.name);
    j.at("type").get_to(sub_domain.type);
    j.at("interface").get_to(sub_domain.interface);
    j.at("ip_source").get_to(sub_domain.ip_source);
    j.at("ip_source_param").get_to(sub_domain.ip_source_param);
    j.at("driver_param").get_to(sub_domain.driver_param);

    auto ip_type = get_optional<ip_version_t>(j, "ip_type");
    sub_domain.ip_type = ip_type.value_or(ip_version_t::UNSPECIFIED);

    auto allow_ula = get_optional<bool>(j, "allow_ula");
    sub_domain.allow_ula = allow_ula.value_or(false);

    auto allow_local_link = get_optional<bool>(j, "allow_local_link");
    sub_domain.allow_local_link = allow_local_link.value_or(false);
}

void Config::from_json(const nlohmann::json &j, domains_config_t &domain) {
    j.at("name").get_to(domain.name);
    j.at("update_interval").get_to(domain.update_interval);
    j.at("force_update").get_to(domain.force_update);
    j.at("driver").get_to(domain.driver);
    j.at("subdomains").get_to(domain.subdomains);
}

void Config::from_json(const nlohmann::json &j, ip_source_t &e) {
    static const std::pair<ip_source_t, nlohmann::json> m[] = {
            {ip_source_t::INTERFACE, "interface"},
            {ip_source_t::URL,       "url"}
    };

    auto it = std::find_if(std::begin(m), std::end(m),
                           [&j](const std::pair<ip_source_t, nlohmann::json> &ej_pair) -> bool {
                               return ej_pair.second == j;
                           });

    e = ((it != std::end(m)) ? it : std::begin(m))->first;
}

NLOHMANN_JSON_SERIALIZE_ENUM(dns_record_t, {
    { dns_record_t::A, "a" },
    { dns_record_t::AAAA, "aaaa" },
    { dns_record_t::TXT, "txt" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(ip_version_t, {
    { ip_version_t::IPV6, "ipv4" },
    { ip_version_t::IPV4, "ipv6" },
    { ip_version_t::UNSPECIFIED, "unspecified" },
})

Config::config_t Config::load_config(std::string_view config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error(fmt::format("config {} not exists", config_path));
    }

    std::ifstream fin(config_path.data());
    if (!fin) {
        throw std::runtime_error("failed to read config file");
    }

    try {
        auto json = nlohmann::json::parse(fin);
        return json.get<Config::config_t>();
    } catch (nlohmann::detail::parse_error &e) {
        throw std::runtime_error(fmt::format("failed to parse config file, error {}", e.what()));
    }
}
