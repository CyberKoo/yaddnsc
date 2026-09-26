//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_CONFIG_CONFIG_H
#define YADDNSC_CONFIG_CONFIG_H

#include <glaze/json/generic_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/network/address_family.h"

enum class RecordKind;

/// Configuration data types.
///
/// This is the RAW JSON DTO (file format): it may carry Glaze types and
/// legacy fields. The normalised, glaze-free runtime model consumed by the
/// business layers lives in src/domain/config/runtime_config.h.
namespace Config {
enum class IpSource;

/// Driver loading configuration.
struct DriverConfig {
    std::optional<std::string> driver_dir{};  ///< Custom directory to search for driver .so files
    bool auto_discover{false};                ///< Automatically discover drivers in the driver directory
    std::vector<std::string> load{};          ///< Explicit list of driver names/paths to load
};

/// DNS resolver configuration.
struct ResolverConfig {
    bool use_custom_server{false};                            ///< Use custom DNS servers instead of system defaults
    std::string address{};                                    ///< Single custom resolver address (backward-compatible)
    unsigned short port{53};                                  ///< Port for the single address (default: 53)
    std::vector<DnsServer> servers{};                         ///< List of custom resolver servers
    ResolverStrategy strategy{ResolverStrategy::CONCURRENT};  ///< Domain Resolve strategy
};

/// Per-subdomain configuration from the config file.
struct SubdomainConfig {
    std::string name{};  ///< Subdomain label (e.g. "www", "@" for apex)
    /// DNS record type to update. Disengaged when the key is absent from
    /// the config file — the normaliser then falls back to A (with a
    /// warning) so legacy configs keep working.
    std::optional<RecordKind> type{};
    std::string interface{};                            ///< Network interface name (for INTERFACE IP source)
    AddressFamily ip_type{AddressFamily::UNSPECIFIED};  ///< Preferred address family
    IpSource ip_source{};                               ///< IP source backend
    std::string ip_source_param{};  ///< Parameter passed to the IP source (URL, mDNS hostname, etc.)
    bool allow_ula{false};          ///< Allow Unique Local Address (ULA, fc00::/7)
    bool allow_local_link{false};   ///< Allow link-local addresses (fe80::/10)
    int update_interval{};          ///< Per-subdomain override of the domain update interval (0 = inherit)
    glz::generic driver_param{};    ///< Driver-specific JSON configuration
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
    DriverConfig driver{};                ///< Driver loading configuration
    ResolverConfig resolver{};            ///< DNS resolver configuration
    std::vector<DomainConfig> domains{};  ///< Domains to manage
    /// Bootstrap DNS server (IP literal, port 53) used to resolve hostname
    /// targets of outbound connections (DoH/DoT servers, HTTP IP sources,
    /// provider APIs). Empty: fall back to /etc/resolv.conf nameservers.
    /// getaddrinfo/NSS is never used.
    std::string bootstrap_dns{};
};

/// Load the application configuration from a JSON file.
/// @param config_path  Path to the JSON configuration file.
/// @return             Parsed AppConfig struct.
AppConfig load_config(const std::string& config_path);

/// Serialize a diagnostic view of a config with sensitive driver fields redacted.
[[nodiscard]] std::string redacted_json(AppConfig config);
}  // namespace Config

#endif  // YADDNSC_CONFIG_CONFIG_H
