#include "environment_validator.h"

#include <algorithm>
#include <optional>
#include <ranges>  // IWYU pragma: keep — IWYU attributes std::ranges::find (__find_fn) here; clangd attributes it to <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <yaddnsc/util/format.hpp>

#include "application/ports/driver_catalog.h"
#include "application/ports/network_interfaces.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "support/fmt.hpp"

namespace app {

std::expected<void, std::vector<domain::ConfigError>> validate_environment(const domain::RuntimeConfig& config,
                                                                           const DriverCatalogPort& catalog,
                                                                           const NetworkInterfacesPort& interfaces) {
    const auto loaded_drivers = catalog.loaded_drivers();
    const auto available_interfaces = interfaces.names();

    // Collect every violation instead of returning the first one: a startup
    // that reports one broken domain at a time makes a configuration mistake
    // cost one restart per mistake.
    std::vector<domain::ConfigError> errors;
    for (const auto& domain_config : config.domains) {
        // --- Check that the referenced driver is loaded. ---------------------
        const bool driver_loaded = std::ranges::find(loaded_drivers, domain_config.driver) != loaded_drivers.end();
        if (!driver_loaded) {
            errors.push_back({domain::ConfigError::Code::DRIVER_NOT_FOUND,
                              fmt::format("Driver {} not found", domain_config.driver)});
        }

        // Capabilities come from the descriptor, so they are only knowable for
        // a driver that actually loaded. A failed describe() leaves the
        // record-type check to the update-time gate.
        std::optional<app::DriverDescription> description;
        if (driver_loaded) {
            if (auto queried = catalog.describe(domain_config.driver); queried.has_value()) {
                description = std::move(*queried);
            }
        }

        for (const auto& subdomain : domain_config.subdomains) {
            // --- Check that every referenced interface exists. ---------------
            if (!subdomain.interface.empty() &&
                std::ranges::find(available_interfaces, subdomain.interface) == available_interfaces.end()) {
                errors.push_back({domain::ConfigError::Code::INTERFACE_NOT_FOUND,
                                  fmt::format("Interface {} not found, available interfaces: {}", subdomain.interface,
                                              fmt::join(available_interfaces, ", "))});
            }

            // --- Check that the driver can publish this record type. ---------
            // The capability gate is host-enforced (see the ABI capability
            // bits); reporting it here turns a silent per-cycle failure into a
            // startup configuration error.
            if (description.has_value() && std::ranges::find(description->supported_records, subdomain.type) ==
                                               description->supported_records.end()) {
                errors.push_back(
                    {domain::ConfigError::Code::DRIVER_UNSUPPORTED_RECORD,
                     fmt::format("Driver {} does not support record type {} required by {}.{}", domain_config.driver,
                                 domain::record_kind_to_str(subdomain.type), domain_config.name, subdomain.name)});
            }
        }
    }

    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return {};
}

}  // namespace app
