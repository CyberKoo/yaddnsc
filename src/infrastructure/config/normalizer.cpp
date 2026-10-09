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

#include "resolver_config.h"

namespace Config {

namespace {
auto normalize_resolver(const ResolverConfig& raw) -> domain::ResolverSettings {
    domain::ResolverSettings settings;
    settings.strategy = raw.strategy;

    if (raw.use_custom_servers) {
        settings.servers = raw.servers;
    } else {
        settings.servers.push_back({YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT});
    }

    return settings;
}

auto normalize_subdomain(const SubdomainConfig& raw, int domain_interval) -> domain::SubdomainConfig {
    if (!raw.type.has_value()) {
        SPDLOG_WARN("Subdomain {} has no record type configured, treating it as an A record", raw.name);
    }
    if (!raw.ip_source.has_value()) {
        SPDLOG_WARN("Subdomain {} has no ip_source configured, defaulting to 'interface'", raw.name);
    }
    return {
        .name = raw.name,
        .type = raw.type.value_or(domain::RecordKind::A),
        .interface = raw.interface,
        .ip_source = raw.ip_source.value_or(domain::IpSource::INTERFACE),
        .ip_source_param = raw.ip_source_param,
        .allow_ula = raw.allow_ula,
        .allow_local_link = raw.allow_local_link,
        .update_interval = raw.update_interval > 0 ? raw.update_interval : domain_interval,
        // An unset driver_params arrives as glz::generic null (either the
        // key is absent or explicitly null); the driver must receive "{}".
        .driver_params = raw.driver_params.is_null() ? "{}" : raw.driver_params.dump().value_or("{}"),
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

    if (!raw.bootstrap_dns.empty()) {
        config.resolver.bootstrap_servers.push_back({raw.bootstrap_dns, YADDNSC_DEFAULT_DNS_PORT});
    }

    if (raw.drivers.driver_dir.has_value()) {
        config.drivers.driver_dir = std::filesystem::path{*raw.drivers.driver_dir};
    }
    config.drivers.auto_discover = raw.drivers.auto_discover;
    config.drivers.load = raw.drivers.load;

    config.domains.reserve(raw.domains.size());
    for (const auto& domain_config : raw.domains) {
        config.domains.push_back(normalize_domain(domain_config));
    }

    return config;
}

}  // namespace Config
