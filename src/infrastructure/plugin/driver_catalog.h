#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "application/ports/driver_catalog.h"

namespace plugin {
class PluginPackage;
}

/// Name-to-package registry. Loading lives in DriverLoader; the catalog only
/// owns registration, lookup and translation to the application query port.
///
/// Thread safety: mutation is not thread-safe and is restricted to startup or
/// teardown. register_package() and unload_driver() must be serialized with
/// every lookup. Once the run loop starts, the catalog is read-only and all
/// lookups are loop-thread only. A package lease extends the loaded image's
/// lifetime; it does not synchronize access to the map itself.
class DriverCatalog : public app::DriverCatalogPort {
public:
    DriverCatalog() = default;
    explicit DriverCatalog(std::vector<std::shared_ptr<const plugin::PluginPackage>> packages);

    /// Register a validated package. A duplicate name keeps the first package.
    void register_package(std::shared_ptr<const plugin::PluginPackage> package);

    /// Remove a package from the registry. In-flight sessions retain a lease.
    /// @throws DriverNotFoundException if no package has the given name.
    void unload_driver(const std::string& name);

    /// Look up a loaded package; nullptr when absent.
    [[nodiscard]] std::shared_ptr<const plugin::PluginPackage> find(std::string_view name) const;

    /// Application port: names of all currently loaded packages.
    [[nodiscard]] std::vector<std::string> loaded_drivers() const override;

    /// Application port: a domain-facing description with no raw ABI state.
    [[nodiscard]] std::expected<app::DriverDescription, domain::DriverError> describe(
        std::string_view name) const override;

private:
    std::map<std::string, std::shared_ptr<const plugin::PluginPackage>, std::less<>> modules_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_CATALOG_H
