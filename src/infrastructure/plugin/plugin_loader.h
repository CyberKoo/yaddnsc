//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <exception>
#include <expected>
#include <string>
#include <string_view>

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
/// four resolved required entry points, the optional validate entry, and the
/// validated descriptor.
///
/// Loading follows the ABI protocol order: dlopen(RTLD_NOW|RTLD_LOCAL) →
/// resolve all required entry points → get_descriptor() → magic check → exact
/// api_revision match → descriptor minimum struct_size check → copy
/// descriptor fields. Only then may create() be called. The fifth entry
/// (validate) is optional: it is dlsym-probed and simply stays nullptr when
/// the plugin predates it.
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

    /// Entry-point trampolines — thin forwards into the plugin, behind an
    /// exception firewall: the ABI forbids exceptions, but a misbehaving
    /// third-party plugin must not let one escape its C frame into the host.
    ///
    /// create() additionally enforces the handle-ownership contract out of
    /// line (plugin_loader.cpp): a failure return must leave *out_driver
    /// null, and a handle stored before the failure is destroyed and
    /// cleared by the host.
    [[nodiscard]] yaddnsc_status create(const yaddnsc_host_services &services, yaddnsc_driver **out_driver,
                                        yaddnsc_error &out_error) const;

    /// Destroy behind a noexcept firewall. A broken third-party destroy
    /// entry point must never escape through DriverInstance's destructor.
    void destroy(yaddnsc_driver *driver) const noexcept;

    [[nodiscard]] yaddnsc_status update(yaddnsc_driver *driver, const yaddnsc_update_request &request,
                                        yaddnsc_error &out_error) const {
        try {
            return update_(driver, &request, &out_error);
        } catch (const std::exception &e) {
            write_entry_error(out_error, e.what());
        } catch (...) {
            write_entry_error(out_error, "unknown exception from plugin update");
        }
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }

    /// Whether the plugin exports the OPTIONAL yaddnsc_driver_validate entry
    /// (added within api_revision 1). Plugins built against an older SDK do
    /// not export it and are simply skipped during config validation.
    [[nodiscard]] bool supports_validate() const noexcept { return validate_ != nullptr; }

    /// Validate a driver_param JSON against the plugin's schema, behind the
    /// same exception firewall as the other trampolines. When the plugin
    /// does not export the optional entry this returns OK — the caller must
    /// treat that as "no driver-side validation", never as an error.
    [[nodiscard]] yaddnsc_status validate(yaddnsc_driver *driver, yaddnsc_string driver_param_json,
                                          yaddnsc_error &out_error) const {
        if (validate_ == nullptr) {
            return YADDNSC_STATUS_OK;
        }
        try {
            return validate_(driver, driver_param_json, &out_error);
        } catch (const std::exception &e) {
            write_entry_error(out_error, e.what());
        } catch (...) {
            write_entry_error(out_error, "unknown exception from plugin validate");
        }
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }

private:
    PluginModule() = default;

    /// Report a firewall-caught exception through the ABI error struct,
    /// honouring the caller-supplied struct_size.  The bounded thread-local
    /// storage makes this noexcept path allocation-free: a plugin exception
    /// must never turn into a second termination while reporting it.
    static void write_entry_error(yaddnsc_error &out_error, std::string_view message) noexcept {
        if (out_error.struct_size < YADDNSC_ERROR_MIN_SIZE) {
            return;
        }
        constexpr std::string_view fallback = "plugin entry threw";
        constexpr std::size_t capacity = 512;
        thread_local std::array<char, capacity> storage{};
        const std::string_view source = message.data() == nullptr ? fallback : message;
        const std::size_t size = std::min(source.size(), storage.size() - 1);
        if (size != 0) {
            std::memcpy(storage.data(), source.data(), size);
        }
        storage[size] = '\0';
        out_error.status = YADDNSC_STATUS_INTERNAL_ERROR;
        out_error.retry_after_seconds = 0;
        out_error.message = yaddnsc_string{storage.data(), size};
        out_error.struct_size = out_error.struct_size < static_cast<std::uint32_t>(sizeof(yaddnsc_error))
                                        ? out_error.struct_size
                                        : static_cast<std::uint32_t>(sizeof(yaddnsc_error));
    }

    SharedLibrary library_;
    decltype(&yaddnsc_driver_get_descriptor) get_descriptor_ = nullptr;
    decltype(&yaddnsc_driver_create) create_ = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy_ = nullptr;
    decltype(&yaddnsc_driver_update) update_ = nullptr;
    decltype(&yaddnsc_driver_validate) validate_ = nullptr;
    DriverDescriptor descriptor_;
};

#endif // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
