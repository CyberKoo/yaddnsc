//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_DRIVER_LOADER_H
#define YADDNSC_CORE_DRIVER_LOADER_H

namespace domain {
    struct DriverSettings;
}

class DriverCatalog;

/// DriverLoader — loads all configured DDNS driver plugin shared libraries.
///
/// Extracted from Manager::Impl to keep driver-loading logic independent of
/// the scheduler and signal-handling concerns.
struct DriverLoader {
    /// Load all drivers specified in the driver settings.
    /// @param driver_catalog  The catalog to register loaded drivers into.
    /// @param settings        Driver loading settings (directory, discovery, load list).
    static void load(DriverCatalog &driver_catalog, const domain::DriverSettings &settings);
};

#endif // YADDNSC_CORE_DRIVER_LOADER_H
