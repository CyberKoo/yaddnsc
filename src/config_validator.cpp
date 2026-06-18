//
// Created by Kotarou on 2026/6/18.
//

#include "config_validator.h"

#include "fmt.h"

#include <config_cmake.h>

#include "ip_util.h"
#include "driver_manager.h"
#include "network_manager.h"

#include "exception/config_verification_exception.h"

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
#ifdef HAVE_RES_NQUERY
    if (cfg.resolver.use_custom_server) {
        const auto &address = cfg.resolver.ip_address;
#ifdef HAVE_IPV6_RESOLVE_SUPPORT
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
    }
#endif
}

// --- Explicit instantiation ------------------------------------------------

template class ConfigValidator<60>;
