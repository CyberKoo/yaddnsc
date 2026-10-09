#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "application/ports/driver_catalog.h"
#include "infrastructure/plugin/plugin_loader.h"

/// DriverCatalog — name → loaded-plugin registry (replaces DriverManager).
///
/// Modules are held through shared_ptr so that an in-flight update keeps its
/// module alive (a lease) even if the entry is concurrently removed from the
/// catalog.
///
/// The application layer sees the catalog through the app::DriverCatalogPort
/// interface (loaded_drivers / describe); load/unload stay on this class.
///
/// @note Not thread-safe for mutation, same as the legacy DriverManager: the
///       catalog is populated during initialisation and read-only during the
///       run loop.
class DriverCatalog : public app::DriverCatalogPort {
public:
    /// Load a driver plugin from the given filesystem path.
    /// A duplicate driver name is skipped with a warning (kept behaviour).
    /// @throws PluginLoadException  When the library fails to load or fails
    ///                              the ABI checks.
    void load_driver(const std::string& path);

    /// Unload a previously loaded driver by name.
    /// @throws DriverNotFoundException  If no driver with that name is loaded.
    void unload_driver(const std::string& name);

    /// Return the names of all currently loaded drivers (owned copies).
    [[nodiscard]] std::vector<std::string> get_loaded_drivers() const;

    /// Look up a loaded module by driver name; nullptr when absent.
    [[nodiscard]] std::shared_ptr<const PluginModule> find(std::string_view name) const;

    /// Look up a loaded driver's descriptor.
    /// @throws DriverNotFoundException  If no driver with that name is loaded.
    [[nodiscard]] const DriverDescriptor& get_descriptor(std::string_view name) const;

    /// app::DriverCatalogPort: names of all currently loaded drivers.
    [[nodiscard]] std::vector<std::string> loaded_drivers() const override;

    /// app::DriverCatalogPort: value-copy description of one loaded driver.
    /// Failure: DriverError::NOT_FOUND when absent; allocation failures propagate.
    [[nodiscard]] std::expected<app::DriverDescription, domain::DriverError> describe(
        std::string_view name) const override;

private:
    std::map<std::string, std::shared_ptr<const PluginModule>, std::less<>> modules_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H
