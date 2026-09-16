//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_RUNTIME_CONFIG_H
#define YADDNSC_DOMAIN_RUNTIME_CONFIG_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "address_family.h"
#include "ip_source_kind.h"
#include "record_kind.h"

#include "config/dns_config.h"

/// Domain runtime configuration model.
///
/// Normalised, glaze-free value objects produced by the config adapter
/// (src/config/normalizer + static_validator). Unlike the raw DTO in
/// src/config/config.h, this model contains:
///   - no glz::generic (driver_param is opaque JSON text),
///   - no legacy file-format fields (resolver is already a server list),
///   - no CLI11 types and no environment probe results.
/// Effective values (e.g. the per-subdomain update interval after applying
/// the domain fallback) are precomputed during normalisation.
namespace domain {

/// Driver loading settings (normalised view of the raw "driver" section).
struct DriverSettings {
    std::optional<std::filesystem::path> driver_dir; ///< Custom driver directory
    bool auto_discover{false};                       ///< Discover all .so files in the directory
    std::vector<std::string> load;                   ///< Explicit driver names/paths to load
};

/// DNS resolver settings with legacy fields already folded in.
/// An empty `servers` list means "use the built-in default"; injecting the
/// default server stays in the infrastructure factory (it is build-configured
/// and logged at startup).
struct ResolverSettings {
    std::vector<Config::DnsServer> servers;                       ///< Normalised server list
    Config::ResolverStrategy strategy{Config::ResolverStrategy::CONCURRENT}; ///< Resolution strategy
};

/// Per-subdomain runtime configuration.
struct SubdomainConfig {
    std::string name;                    ///< Subdomain label (e.g. "www", "@" for apex)
    RecordKind type{};                   ///< DNS record type to update
    std::string interface;               ///< Network interface name (INTERFACE source / HTTP bind)
    AddressFamily ip_type{AddressFamily::UNSPECIFIED}; ///< Preferred address family
    Config::IpSource ip_source{};        ///< IP source backend
    std::string ip_source_param;         ///< IP source parameter (URL, mDNS hostname, ...)
    bool allow_ula{false};               ///< Allow ULA (fc00::/7) for AAAA
    bool allow_local_link{false};        ///< Allow link-local (fe80::/10) for AAAA
    int update_interval{};               ///< EFFECTIVE interval (subdomain override or domain value)
    std::string driver_param;            ///< Opaque JSON text for the driver (fields/values preserved)
};

/// Per-domain runtime configuration.
struct DomainConfig {
    std::string name;                          ///< Domain name (e.g. "example.com")
    int update_interval{};                     ///< Domain-level update interval (seconds)
    int force_update{};                        ///< Force-update interval in seconds (0 = disabled)
    std::string driver;                        ///< Name of the driver plugin to use
    std::vector<SubdomainConfig> subdomains;   ///< Subdomains to update
};

/// Top-level runtime configuration.
struct RuntimeConfig {
    DriverSettings driver;               ///< Driver loading settings
    ResolverSettings resolver;           ///< DNS resolver settings
    std::vector<DomainConfig> domains;   ///< Domains to manage
};

} // namespace domain

#endif // YADDNSC_DOMAIN_RUNTIME_CONFIG_H
