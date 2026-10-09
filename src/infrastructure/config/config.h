#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_H

#include <optional>
#include <string>
#include <vector>

#include <glaze/json/generic_fwd.hpp>

#include "domain/config/dns_config.h"

namespace domain {
enum class RecordKind;
enum class IpSource;
}  // namespace domain

/// Configuration data types.
///
/// This is the RAW JSON DTO (file format): it may carry Glaze types and
/// legacy fields. The normalised, glaze-free runtime model consumed by the
/// business layers lives in src/domain/config/runtime_config.h.
namespace Config {

/// Driver loading configuration.
struct DriverConfig {
    std::optional<std::string> driver_dir{};  ///< Custom directory to search for driver .so files
    bool auto_discover{false};                ///< Automatically discover drivers in the driver directory
    std::vector<std::string> load{};          ///< Explicit list of driver names/paths to load
};

/// DNS resolver configuration.
struct ResolverConfig {
    bool use_custom_servers{false};                           ///< Use custom DNS servers instead of system defaults
    std::vector<domain::DnsServer> servers{};                 ///< List of custom resolver servers
    domain::ResolverStrategy strategy{domain::ResolverStrategy::CONCURRENT};  ///< Domain Resolve strategy
};

/// Per-subdomain configuration from the config file.
struct SubdomainConfig {
    std::string name{};  ///< Subdomain label (e.g. "www", "@" for apex)
    /// DNS record type to update. Disengaged when the key is absent from
    /// the config file — the normaliser then falls back to A (with a
    /// warning) so legacy configs keep working.
    std::optional<domain::RecordKind> type{};
    std::string interface{};  ///< Network interface name (for INTERFACE IP source)
    /// IP source backend. Disengaged when the key is absent from the config
    /// file — the normaliser then falls back to INTERFACE (with a warning).
    std::optional<domain::IpSource> ip_source{};
    std::string ip_source_param{};  ///< Parameter passed to the IP source (URL, mDNS hostname, etc.)
    bool allow_ula{false};          ///< Allow Unique Local Address (ULA, fc00::/7)
    bool allow_local_link{false};   ///< Allow link-local addresses (fe80::/10)
    int update_interval{};          ///< Per-subdomain override of the domain update interval (0 = inherit)
    glz::generic driver_params{};   ///< Driver-specific JSON configuration
};

/// Per-domain configuration from the config file.
struct DomainConfig {
    std::string name{};                         ///< Domain name (e.g. "example.com")
    int update_interval{};                      ///< Update interval in seconds
    int force_update{};                         ///< Force-update interval in seconds (0 = disabled)
    std::string driver{};                       ///< Name of the driver plugin to use
    std::vector<SubdomainConfig> subdomains{};  ///< Subdomains to update
};

/// Top-level application configuration.
struct AppConfig {
    DriverConfig drivers{};               ///< Driver loading configuration
    ResolverConfig resolver{};            ///< DNS resolver configuration
    std::vector<DomainConfig> domains{};  ///< Domains to manage
    /// Bootstrap DNS server (IP literal, port 53) used to resolve hostname
    /// targets of outbound connections (DoH/DoT servers, HTTP IP sources,
    /// provider APIs). Empty: fall back to /etc/resolv.conf nameservers.
    std::string bootstrap_dns{};
};

/// Load the application configuration from a JSON file.
/// @param config_path  Path to the JSON configuration file.
/// @return             Parsed AppConfig struct.
AppConfig load_config(const std::string& config_path);

/// Serialize a diagnostic view of a config with sensitive driver fields redacted.
[[nodiscard]] std::string redacted_json(AppConfig config);
}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_H
