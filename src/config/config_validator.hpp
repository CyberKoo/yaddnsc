//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP
#define YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP

#include <ranges>
#include <cctype>

#include "uri.h"
#include "fmt.hpp"
#include "config.h"
#include "mixin.h"
#include "dns/types.h"
#include "config_cmake.h"
#include "network/inet_address.h"
#include "core/driver_manager.h"
#include "exceptions/config_verification_exception.h"

class DriverManager;

// ---------------------------------------------------------------------------
// Internal helpers (hidden in detail namespace)
// ---------------------------------------------------------------------------
namespace detail {
    inline std::string fqdn_for(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        return fmt::format("{}.{}", subdomain.name, domain.name);
    }

    inline void validate_ip_source(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        auto fqdn = fqdn_for(domain, subdomain);

        // Both HTTP and INTERFACE sources require a non-empty interface name.
        if (subdomain.interface.empty()) {
            throw ConfigVerificationException(
                fmt::format("Subdomain {} requires a network interface (field 'interface' is empty or missing)", fqdn)
            );
        }

        if (subdomain.ip_source == Config::ip_source_type::HTTP) {
            if (subdomain.ip_source_param.empty()) {
                throw ConfigVerificationException(
                    fmt::format("Subdomain {} uses HTTP IP source but ip_source_param is empty", fqdn)
                );
            }

            try {
                const auto uri = Uri::parse(subdomain.ip_source_param);
                if (uri.get_host().empty() || uri.get_port() == 0) {
                    throw std::runtime_error("missing host or port");
                }
            } catch (const std::exception &e) {
                throw ConfigVerificationException(
                    fmt::format("Subdomain {} has invalid ip_source_param '{}': {}", fqdn, subdomain.ip_source_param,
                                e.what())
                );
            }
            return;
        }
    }

    inline void validate_resolver_address(const std::string &address) {
        const auto uri = Uri::parse(address);
        // DoH / DoT address — starts with https or tls, skip IP validation.
        if (uri.get_schema() == "https" || uri.get_schema() == "tls") {
            return;
        }

        // Plain DNS address — must be a valid IP.
#if defined(HAVE_RES_NQUERY)
#if defined(HAVE_IPV6_RESOLVE_SUPPORT)
        if (!InetAddress::parse(address)) {
            throw ConfigVerificationException(fmt::format("Invalid resolver address {}", address));
        }
#else
        if (!Inet4Address::parse(address)) {
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
// interval (in seconds).  Interface names are injected as a vector so that the
// validator can check that every referenced interface exists on the system.
//
// Each check throws ConfigVerificationException on failure, so a single call
// to validate() either returns normally (all checks pass) or throws at the
// first violated constraint.
// ---------------------------------------------------------------------------
template<int UpdateInterval>
class ConfigValidator {
public:
    ConfigValidator(const DriverManager &driver_manager, std::vector<std::string> interfaces)
        : driver_manager_(driver_manager), interfaces_(std::move(interfaces)) {
    }

    void validate(const Config::config &cfg) const {
        const auto drivers = driver_manager_.get_loaded_drivers();

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

                if (std::ranges::find(interfaces_, subdomain.interface) == interfaces_.end()) {
                    auto available = fmt::format("{}", fmt::join(interfaces_, ", "));
                    throw ConfigVerificationException(
                        fmt::format("Interface {} not found, available interfaces: {}", subdomain.interface,
                                    available)
                    );
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
    const std::vector<std::string> interfaces_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CONFIG_CONFIG_VALIDATOR_HPP
