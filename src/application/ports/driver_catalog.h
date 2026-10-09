#ifndef YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H
#define YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H

#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/error/error.h"

namespace app {

/// User-visible description of one loaded driver plugin (a plain value copy
/// of the plugin's descriptor, so the port never hands out references into
/// plugin-owned memory).
struct DriverDescription {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
};

/// DriverCatalogPort — application port for querying which driver plugins
/// are loaded.
///
/// The concrete DriverCatalog (infrastructure) implements this port; the
/// environment validator and the `driver list` / `driver info` commands
/// consume it. Mutating operations (load/unload) stay on the concrete
/// catalog and never cross this boundary.
///
/// Synchronous queries; populate the catalog before querying and do not mutate
/// it concurrently. Returned descriptions own their strings. A missing driver
/// is DriverError::NOT_FOUND with no message; the caller supplies the queried
/// name to its presenter. Allocation failures propagate as exceptions.
class DriverCatalogPort {
public:
    virtual ~DriverCatalogPort() = default;

    /// Names of all currently loaded drivers (owned copies).
    [[nodiscard]] virtual std::vector<std::string> loaded_drivers() const = 0;

    /// Description of one loaded driver.
    /// Failure: DriverError::NOT_FOUND when the driver is not loaded.
    [[nodiscard]] virtual std::expected<DriverDescription, domain::DriverError> describe(
        std::string_view name) const = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H
