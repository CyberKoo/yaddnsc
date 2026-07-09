//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CORE_DRIVER_MANAGER_H
#define YADDNSC_CORE_DRIVER_MANAGER_H

#include <memory>
#include <string>
#include <vector>
#include <string_view>

#include "mixin.h"

class Driver;

/// Abstract interface for driver lifecycle management.
///
/// Provides a test-friendly seam so that components like ConfigValidator
/// can be tested with a MockDriverManager instead of a real (dlopen-based)
/// DriverManager.
///
/// @see DriverManager  (production implementation)
/// @see MockDriverManager  (test double)
class DriverManagerBase {
public:
    virtual ~DriverManagerBase() = default;

    /// Load a driver shared library from the given filesystem path.
    /// @param path  Path to the .so file to load.
    virtual void load_driver(const std::string &path) const = 0;

    /// Unload a previously loaded driver by name.
    /// @param name  Driver name (as returned by DriverDetail::name).
    virtual void unload_driver(const std::string &name) = 0;

    /// Return the names of all currently loaded drivers.
    [[nodiscard]] virtual std::vector<std::string_view> get_loaded_drivers() const = 0;

    /// Look up a loaded driver by name.
    /// @param name  Driver name to look up.
    /// @return      Reference to the Driver instance.
    /// @throws BadDriverException  If no driver with that name is loaded.
    [[nodiscard]] virtual const Driver &get_driver(const std::string &name) const = 0;

protected:
    DriverManagerBase() = default;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

/// Manages the lifecycle of loaded driver plugins.
///
/// Handles dlopen/dlclose, maps driver names to Driver instances, and
/// provides lookup by name for the updater and CLI components.
///
/// Inherits from DriverManagerBase to support mock-based testing.
///
/// @note Not thread-safe: the driver map is populated during initialisation
///       and read-only during the run loop.  Callers must serialise calls to
///       load_driver() / unload_driver() with any concurrent access.
class DriverManager final : public DriverManagerBase {
public:
    DriverManager();

    ~DriverManager() override;

    /// Load a driver shared library from the given filesystem path.
    /// @param path  Path to the .so file to load.
    void load_driver(const std::string &path) const override;

    /// Unload a previously loaded driver by name.
    /// @param name  Driver name (as returned by DriverDetail::name).
    void unload_driver(const std::string &name) override;

    /// Return the names of all currently loaded drivers.
    [[nodiscard]] std::vector<std::string_view> get_loaded_drivers() const override;

    /// Look up a loaded driver by name.
    /// @param name  Driver name to look up.
    /// @return      Reference to the Driver instance.
    /// @throws BadDriverException  If no driver with that name is loaded.
    [[nodiscard]] const Driver &get_driver(const std::string &name) const override;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_CORE_DRIVER_MANAGER_H
