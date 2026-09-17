//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H
#define YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H

#include <string>
#include <string_view>
#include <vector>

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
/// Error contract: describe() throws DriverNotFoundException when the driver
/// is not loaded — the legacy wording is preserved verbatim for the CLI's
/// "Error: ..." output.
class DriverCatalogPort {
public:
    virtual ~DriverCatalogPort() = default;

    /// Names of all currently loaded drivers (owned copies).
    [[nodiscard]] virtual std::vector<std::string> loaded_drivers() const = 0;

    /// Description of one loaded driver.
    /// @throws DriverNotFoundException  If no driver with that name is loaded.
    [[nodiscard]] virtual DriverDescription describe(std::string_view name) const = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_DRIVER_CATALOG_H
