//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CONFIG_VALIDATOR_HPP
#define YADDNSC_CONFIG_VALIDATOR_HPP

#include <algorithm>
#include <string>
#include <vector>

#include "fmt.hpp"
#include "mixin.h"

#include "domain/config/runtime_config.h"
#include "exception/config_verification.h"

/// EnvironmentValidator — performs the environment-dependent configuration
/// checks: every referenced driver must be loaded and every referenced
/// network interface must exist on this machine.
///
/// Static (environment-independent) checks live in static_validator.h and
/// produce domain::ConfigError values instead of throwing.
///
/// Fail-fast: validate() throws ConfigVerificationException on the first
/// violated constraint, with messages identical to the legacy ConfigValidator.
class EnvironmentValidator {
public:
    /// Construct with loaded driver names and available interfaces.
    /// @param loaded_drivers  Names of currently loaded driver plugins (owned
    ///                        copies, not views into the driver registry).
    /// @param interfaces      List of network interface names on the system.
    EnvironmentValidator(std::vector<std::string> loaded_drivers, std::vector<std::string> interfaces)
        : loaded_drivers_(std::move(loaded_drivers)), interfaces_(std::move(interfaces)) {
    }

    /// Run the environment checks against the runtime configuration.
    /// @param config  The normalised runtime configuration.
    /// @throws ConfigVerificationException  On the first violated constraint.
    void validate(const domain::RuntimeConfig &config) const {
        const auto &drivers = loaded_drivers_;

        for (const auto &domain_config: config.domains) {
            // --- Check that the referenced driver is loaded. -----------------
            if (std::ranges::find(drivers, domain_config.driver) == drivers.end()) {
                throw ConfigVerificationException(fmt::format("Driver {} not found", domain_config.driver));
            }

            // --- Check that every referenced interface exists. ---------------
            for (const auto &subdomain: domain_config.subdomains) {
                if (!subdomain.interface.empty() &&
                    std::ranges::find(interfaces_, subdomain.interface) == interfaces_.end()) {
                    auto available = fmt::format("{}", fmt::join(interfaces_, ", "));
                    throw ConfigVerificationException(
                        fmt::format("Interface {} not found, available interfaces: {}", subdomain.interface,
                                    available
                        )
                    );
                }
            }
        }
    }

private:
    const std::vector<std::string> loaded_drivers_;
    const std::vector<std::string> interfaces_;

    [[maybe_unused, no_unique_address]] NoCopy no_copy_;
    [[maybe_unused, no_unique_address]] NoMove no_move_;
};

#endif // YADDNSC_CONFIG_VALIDATOR_HPP
