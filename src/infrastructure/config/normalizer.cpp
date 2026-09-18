//
// Created by Kotarou on 2026/9/17.
//

#include "normalizer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <spdlog/spdlog.h>

#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"
#include "infrastructure/config/config.h"

namespace Config {

namespace {
auto normalize_resolver(const ResolverConfig& raw) -> domain::ResolverSettings {
    domain::ResolverSettings settings;
    settings.strategy = raw.strategy;

    // Fold the legacy single-server format into the server list. An empty
    // result means "use the built-in default"; the default itself is
    // injected by the infrastructure factory.
    if (raw.use_custom_server) {
        if (!raw.servers.empty()) {
            settings.servers = raw.servers;
        } else if (!raw.address.empty()) {
            settings.servers.push_back({raw.address, raw.port});
        }
    }

    return settings;
}

auto normalize_subdomain(const SubdomainConfig& raw, int domain_interval) -> domain::SubdomainConfig {
    if (!raw.type.has_value()) {
        SPDLOG_WARN("Subdomain {} has no record type configured, treating it as an A record", raw.name);
    }
    return {
        .name = raw.name,
        .type = raw.type.value_or(RecordKind::A),
        .interface = raw.interface,
        .ip_type = raw.ip_type,
        .ip_source = raw.ip_source,
        .ip_source_param = raw.ip_source_param,
        .allow_ula = raw.allow_ula,
        .allow_local_link = raw.allow_local_link,
        .update_interval = raw.update_interval > 0 ? raw.update_interval : domain_interval,
        // An unset driver_param arrives as glz::generic null (either the
        // key is absent or explicitly null); the driver must receive "{}".
        .driver_param = raw.driver_param.is_null() ? "{}" : raw.driver_param.dump().value_or("{}"),
    };
}

auto normalize_domain(const DomainConfig& raw) -> domain::DomainConfig {
    domain::DomainConfig domain_config{
        .name = raw.name,
        .update_interval = raw.update_interval,
        .force_update = raw.force_update,
        .driver = raw.driver,
        .subdomains = {},
    };
    domain_config.subdomains.reserve(raw.subdomains.size());
    for (const auto& subdomain : raw.subdomains) {
        domain_config.subdomains.push_back(normalize_subdomain(subdomain, raw.update_interval));
    }
    return domain_config;
}
}  // namespace

auto normalize(const AppConfig& raw) -> domain::RuntimeConfig {
    domain::RuntimeConfig config;
    config.resolver = normalize_resolver(raw.resolver);

    if (raw.driver.driver_dir.has_value()) {
        config.driver.driver_dir = std::filesystem::path{*raw.driver.driver_dir};
    }
    config.driver.auto_discover = raw.driver.auto_discover;
    config.driver.load = raw.driver.load;

    config.domains.reserve(raw.domains.size());
    for (const auto& domain_config : raw.domains) {
        config.domains.push_back(normalize_domain(domain_config));
    }

    return config;
}

}  // namespace Config
