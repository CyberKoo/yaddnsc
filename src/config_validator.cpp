//
// Created by Kotarou on 2026/6/18.
//

#include "config_validator.h"

#include "fmt.h"

#include <config_cmake.h>
#include <magic_enum/magic_enum.hpp>

#include "dns.h"
#include "ip_util.h"
#include "driver_manager.h"
#include "network_manager.h"
#include "driver_interface.h"
#include "uri.h"

#include "exception/config_verification_exception.h"

namespace {
    std::string fqdn_for(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        return fmt::format("{}.{}", subdomain.name, domain.name);
    }

    void validate_record_ip_type(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
        const auto expected = DNS::dns2ip(subdomain.type);
        if (expected == address_family::UNSPECIFIED || subdomain.ip_type == address_family::UNSPECIFIED) {
            return;
        }

        if (subdomain.ip_type != expected) {
            throw ConfigVerificationException(
                fmt::format("Subdomain {} has record type/IP type mismatch", fqdn_for(domain, subdomain))
            );
        }
    }

    void validate_ip_source(const Config::domain_config &domain, const Config::subdomain_config &subdomain) {
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

        if (subdomain.interface.empty()) {
            throw ConfigVerificationException(
                fmt::format("Subdomain {} uses interface IP source but interface is empty",
                            fqdn_for(domain, subdomain))
            );
        }
    }

    void validate_resolver_address(const std::string &address) {
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
                fmt::format(R"(Invalid resolver address "{}". Only IPv4 is supported on this platform.)", address));
        }
#endif
#endif
    }
}

// ---------------------------------------------------------------------------
// ConfigValidator
// ---------------------------------------------------------------------------

template<int MinUpdateInterval>
ConfigValidator<MinUpdateInterval>::ConfigValidator(
    const DriverManager &driver_manager,
    const NetworkManager &network_manager) : driver_manager_(driver_manager), network_manager_(network_manager) {
}

template<int MinUpdateInterval>
void ConfigValidator<MinUpdateInterval>::validate(const Config::config &cfg) const {
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
            throw ConfigVerificationException(
                fmt::format("Driver {} not found", driver));
        }

        // --- Check update interval. ------------------------------------------
        if (update_interval < MinUpdateInterval) {
            throw ConfigVerificationException(
                fmt::format("Update interval too low for domain {} ({}), "
                            "minimal interval: {}", name, update_interval, MinUpdateInterval));
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
        for (const auto &subdomain: subdomains) {
            if (subdomain.name.empty()) {
                throw ConfigVerificationException(
                    fmt::format("Subdomain name must not be empty in domain '{}'", name)
                );
            }

            const Config::domain_config domain{name, update_interval, force_update, driver, subdomains};
            validate_ip_source(domain, subdomain);
            validate_record_ip_type(domain, subdomain);

            // --- Validate per-subdomain update_interval if set. --------------
            if (subdomain.update_interval != 0 && subdomain.update_interval < MinUpdateInterval) {
                throw ConfigVerificationException(
                    fmt::format("Update interval too low for subdomain {}.{} ({}), minimal interval: {}",
                                subdomain.name, name, subdomain.update_interval, MinUpdateInterval)
                );
            }

            if (!subdomain.interface.empty()) {
                if (std::ranges::find(interfaces, subdomain.interface) == interfaces.end()) {
                    throw ConfigVerificationException(
                        fmt::format("Interface {} not found", subdomain.interface));
                }
            }
        }
    }

    // --- Validate custom resolver address. ----------------------------------
#if defined(HAVE_RES_NQUERY)
    if (cfg.resolver.use_custom_server) {
        validate_resolver_address(cfg.resolver.ip_address);
    }
#endif
}

// --- Explicit instantiation ------------------------------------------------

template class ConfigValidator<60>;
