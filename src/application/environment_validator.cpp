#include "environment_validator.h"

#include <yaddnsc/util/format.hpp>
#include <algorithm>
#include <expected>
#include <string>
#include <ranges>  // IWYU pragma: keep — IWYU attributes std::ranges::find (__find_fn) here; clangd attributes it to <algorithm>

#include "application/ports/driver_catalog.h"
#include "application/ports/network_interfaces.h"
#include "domain/config/runtime_config.h"
#include "support/fmt.hpp"

namespace app {

std::expected<void, std::vector<domain::ConfigError>> validate_environment(const domain::RuntimeConfig& config,
                                                                           const DriverCatalogPort& catalog,
                                                                           const NetworkInterfacesPort& interfaces) {
    const auto loaded_drivers = catalog.loaded_drivers();
    const auto available_interfaces = interfaces.names();

    for (const auto& domain_config : config.domains) {
        // --- Check that the referenced driver is loaded. ---------------------
        if (std::ranges::find(loaded_drivers, domain_config.driver) == loaded_drivers.end()) {
            return std::unexpected(std::vector<domain::ConfigError>{
                {domain::ConfigError::Code::DRIVER_NOT_FOUND, fmt::format("Driver {} not found", domain_config.driver)},
            });
        }

        // --- Check that every referenced interface exists. -------------------
        for (const auto& subdomain : domain_config.subdomains) {
            if (!subdomain.interface.empty() &&
                std::ranges::find(available_interfaces, subdomain.interface) == available_interfaces.end()) {
                const auto available = fmt::format("{}", fmt::join(available_interfaces, ", "));
                return std::unexpected(std::vector<domain::ConfigError>{
                    {domain::ConfigError::Code::INTERFACE_NOT_FOUND,
                     fmt::format("Interface {} not found, available interfaces: {}", subdomain.interface, available)},
                });
            }
        }
    }

    return {};
}

}  // namespace app
