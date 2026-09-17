//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H

#include <cstdint>
#include <expected>
#include <string>

#include "domain/error/error.h"
#include "shared_library.h"

#include <yaddnsc/sdk/driver_abi.h>

/// DriverDescriptor — host-owned copy of a plugin's static descriptor.
/// All strings are copied out of the module at load time.
struct DriverDescriptor {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::uint64_t capabilities = 0;
    std::uint32_t api_revision = 0;
};

/// PluginModule — one loaded driver plugin: the shared library handle, its
/// four resolved entry points, and the validated descriptor.
///
/// Loading follows the ABI protocol order: dlopen(RTLD_NOW|RTLD_LOCAL) →
/// resolve all entry points → get_descriptor() → magic check → exact
/// api_revision match → descriptor minimum struct_size check → copy
/// descriptor fields. Only then may create() be called.
///
/// @note Thread-safe for concurrent create/update/destroy calls (they only
///       read the entry-point table); load is single-threaded startup work.
class PluginModule {
public:
    /// Load and validate the plugin at @p path.
    [[nodiscard]] static std::expected<PluginModule, domain::PluginError> load(const std::string &path);

    PluginModule(PluginModule &&) noexcept = default;
    PluginModule &operator=(PluginModule &&) noexcept = default;

    PluginModule(const PluginModule &) = delete;
    PluginModule &operator=(const PluginModule &) = delete;

    [[nodiscard]] const DriverDescriptor &descriptor() const noexcept { return descriptor_; }

    [[nodiscard]] const std::string &path() const noexcept { return library_.path(); }

    /// Entry-point trampolines — thin forwards into the plugin.
    [[nodiscard]] yaddnsc_status create(const yaddnsc_host_services &services, yaddnsc_driver **out_driver,
                                        yaddnsc_error &out_error) const {
        return create_(&services, out_driver, &out_error);
    }

    void destroy(yaddnsc_driver *driver) const noexcept { destroy_(driver); }

    [[nodiscard]] yaddnsc_status update(yaddnsc_driver *driver, const yaddnsc_update_request &request,
                                        yaddnsc_error &out_error) const {
        return update_(driver, &request, &out_error);
    }

private:
    PluginModule() = default;

    SharedLibrary library_;
    decltype(&yaddnsc_driver_get_descriptor) get_descriptor_ = nullptr;
    decltype(&yaddnsc_driver_create) create_ = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy_ = nullptr;
    decltype(&yaddnsc_driver_update) update_ = nullptr;
    DriverDescriptor descriptor_;
};

#endif // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
