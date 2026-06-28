//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP
#define YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP

#include "mixin.h"
#include "config.h"

#include "uri.h"
#include "fmt.hpp"
#include "dns/types.h"
#include "config_cmake.h"
#include "network/ip_util.h"
#include "core/driver_manager.h"
#include "network/network_manager.h"
#include "exception/config_verification_exception.h"

class DriverManager;
class NetworkManager;

// ---------------------------------------------------------------------------
// Internal helpers (hidden in detail namespace)
// ---------------------------------------------------------------------------
namespace detail {
    inline std::string fqdn_for(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        return fmt::format("{}.{}", subdomain.name, domain.name);
    }

    inline void validate_ip_source(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        if (subdomain.ip_source == Config::ip_source_type::URL) {
            if (subdomain.ip_source_param.empty()) {
                throw ConfigVerificationException(
                    fmt::format("Subdomain {} uses url IP source but ip_source param is empty",
                                fqdn_for(domain, subdomain))
                );
            }

            try {
                const auto uri = Uri::parse(subdomain.ip_source_param);
                if (uri.get_host().empty() || uri.get_port() == 0) {
                    throw std::runtime_error("missing host or port");
                }
            } catch (const std::exception &e) {
                throw ConfigVerificationException(
                    fmt::format("Subdomain {} has invalid ip_source_param '{}': {}",
                                fqdn_for(domain, subdomain), subdomain.ip_source_param, e.what())
                );
            }
            return;
        }

        if (!subdomain.interface.has_value()) {
            throw ConfigVerificationException(
                fmt::format("Subdomain {} uses interface IP source but interface is empty",
                            fqdn_for(domain, subdomain))
            );
        }
    }

    inline void validate_resolver_address(const std::string &address) {
        // DoH address — starts with https://, skip IP validation.
        if (address.starts_with("https://")) {
            return;
        }

        // Plain DNS address — must be a valid IP.
#if defined(HAVE_RES_NQUERY)
#if defined(HAVE_IPV6_RESOLVE_SUPPORT)
        if (!IPUtil::is_ipv4_address(address) && !IPUtil::is_ipv6_address(address)) {
            throw ConfigVerificationException(
                fmt::format("Invalid resolver address {}", address)
            );
        }
#else
        if (!IPUtil::is_ipv4_address(address)) {
            throw ConfigVerificationException(
                fmt::format(R"(Invalid resolver address "{}". Only IPv4 is supported on this platform.)", address)
            );
        }
#endif
#endif
    }
} // namespace detail

// ---------------------------------------------------------------------------
// ConfigValidator — performs all pre-flight checks on the parsed configuration
// before the scheduler starts.
//
// The MinUpdateInterval template parameter controls the minimum allowed update
// interval (in seconds).  The DriverManager and NetworkManager are injected at
// construction time and must outlive the validator.
//
// Each check throws ConfigVerificationException on failure, so a single call
// to validate() either returns normally (all checks pass) or throws at the
// first violated constraint.
// ---------------------------------------------------------------------------
template<int UpdateInterval>
class ConfigValidator {
public:
    ConfigValidator(const DriverManager &driver_manager, const NetworkManager &network_manager)
        : driver_manager_(driver_manager), network_manager_(network_manager) {
    }

    void validate(const Config::config &cfg) const {
        const auto drivers = driver_manager_.get_loaded_drivers();
        const auto interfaces = network_manager_.get_interfaces();

        for (const auto &[name, update_interval, force_update, driver, subdomains]: cfg.domains) {
            // --- Check domain name is not empty. ---------------------------------
            if (name.empty()) {
                throw ConfigVerificationException("Domain name must not be empty");
            }

            if (subdomains.empty()) {
                throw ConfigVerificationException(
                    fmt::format("Domain '{}' must have at least one subdomain", name));
            }

            // --- Check that the referenced driver is loaded. ---------------------
            if (std::ranges::find(drivers, driver) == drivers.end()) {
                throw ConfigVerificationException(fmt::format("Driver {} not found", driver));
            }

            // --- Check update interval. ------------------------------------------
            if (update_interval < UpdateInterval) {
                throw ConfigVerificationException(
                    fmt::format("Update interval too low for domain {} ({}), minimal interval: {}", name,
                                update_interval, UpdateInterval)
                );
            }

            // --- Check force-update interval. ------------------------------------
            if (force_update != 0 && force_update < update_interval) {
                throw ConfigVerificationException(
                    fmt::format(
                        "Force update interval for domain {} must not be smaller than the update interval ({})",
                        name, update_interval
                    )
                );
            }

            // --- Check that every referenced interface exists. -------------------
            // Build a domain_config once so we don't copy the subdomains vector
            // on every iteration of the inner loop.
            const Config::domain_config domain{name, update_interval, force_update, driver, subdomains};
            for (const auto &subdomain: subdomains) {
                if (subdomain.name.empty()) {
                    throw ConfigVerificationException(
                        fmt::format("Subdomain name must not be empty in domain '{}'", name)
                    );
                }

                detail::validate_ip_source(domain, subdomain);

                // --- Validate per-subdomain update_interval if set. --------------
                if (subdomain.update_interval != 0 && subdomain.update_interval < UpdateInterval) {
                    throw ConfigVerificationException(
                        fmt::format("Update interval too low for subdomain {}.{} ({}), minimal interval: {}",
                                    subdomain.name, name, subdomain.update_interval, UpdateInterval)
                    );
                }

                if (subdomain.interface.has_value()) {
                    if (std::ranges::find(interfaces, *subdomain.interface) == interfaces.end()) {
                        auto available = fmt::format("{}", fmt::join(interfaces, ", "));
                        throw ConfigVerificationException(
                            fmt::format("Interface {} not found, available interfaces: {}", *subdomain.interface,
                                        available)
                        );
                    }
                }
            }
        }

        // --- Validate custom resolver address(es). --------------------------------
#if defined(HAVE_RES_NQUERY)
        if (cfg.resolver.use_custom_server) {
            if (!cfg.resolver.servers.empty()) {
                for (const auto &server: cfg.resolver.servers) {
                    detail::validate_resolver_address(server.address);
                }
            } else if (!cfg.resolver.address.empty()) {
                detail::validate_resolver_address(cfg.resolver.address);
            }
        }
#endif
    }

private:
    const DriverManager &driver_manager_;
    const NetworkManager &network_manager_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP
