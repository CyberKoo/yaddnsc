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

/// Manages the lifecycle of loaded driver plugins.
///
/// Handles dlopen/dlclose, maps driver names to Driver instances, and
/// provides lookup by name for the updater and CLI components.
class DriverManager final {
public:
    DriverManager();

    ~DriverManager();

    /// Load a driver shared library from the given filesystem path.
    /// @param path  Path to the .so file to load.
    void load_driver(const std::string &path) const;

    /// Unload a previously loaded driver by name.
    /// @param name  Driver name (as returned by DriverDetail::name).
    void unload_driver(const std::string &name);

    /// Return the names of all currently loaded drivers.
    [[nodiscard]] std::vector<std::string_view> get_loaded_drivers() const;

    /// Look up a loaded driver by name.
    /// @param name  Driver name to look up.
    /// @return      Reference to the Driver instance.
    /// @throws BadDriverException  If no driver with that name is loaded.
    [[nodiscard]] const Driver &get_driver(const std::string &name) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_CORE_DRIVER_MANAGER_H
