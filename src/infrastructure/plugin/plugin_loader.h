#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H

#include <cstdint>
#include <string>
#include <string_view>

#include <expected>
#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/plugin_error.h"
#include "infrastructure/plugin/shared_library.h"

/// DriverDescriptor — host-owned copy of a plugin's static descriptor.
/// All strings are copied out of the module at load time.
struct DriverDescriptor {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::uint64_t capabilities = 0;
    std::uint16_t abi_major = 0;
    std::uint16_t abi_minor = 0;
};

/// PluginModule — one loaded driver plugin: the shared library handle, its
/// four resolved required entry points, the optional validate entry, and the
/// validated descriptor.
///
/// Loading follows the ABI protocol order: dlopen(RTLD_NOW|RTLD_LOCAL) →
/// resolve all required entry points → get_descriptor() → version prefix →
/// abi_major/abi_minor (`yaddnsc_abi_provides`) → minor baseline struct_size →
/// magic, strings, and capability bits → copy descriptor fields. Only then
/// may create() be called. The fifth entry (validate) is optional: it is
/// dlsym-probed and simply stays nullptr when the plugin does not export it.
///
/// @note Thread-safe for concurrent create/update/destroy calls (they only
///       read the entry-point table); load is single-threaded startup work.
class PluginModule {
public:
    /// Load and validate the plugin at @p path.
    [[nodiscard]] static std::expected<PluginModule, plugin::PluginError> load(const std::string& path);

    PluginModule(PluginModule&&) noexcept = default;
    PluginModule& operator=(PluginModule&&) noexcept = default;

    PluginModule(const PluginModule&) = delete;
    PluginModule& operator=(const PluginModule&) = delete;

    [[nodiscard]] const DriverDescriptor& descriptor() const noexcept { return descriptor_; }

    [[nodiscard]] const std::string& path() const noexcept { return library_.path(); }

    /// Entry-point trampolines — thin forwards into the plugin, behind an
    /// exception firewall: the ABI forbids exceptions, but a misbehaving
    /// third-party plugin must not let one escape its C frame into the host.
    ///
    /// create() additionally enforces the handle-ownership contract out of
    /// line (plugin_loader.cpp): *out_driver is cleared before the call, a
    /// failure return leaves it null (a handle stored before the failure is
    /// destroyed), and OK with a null handle becomes INTERNAL_ERROR.
    [[nodiscard]] yaddnsc_status create(const yaddnsc_host_services& services, yaddnsc_driver** out_driver,
                                        yaddnsc_error& out_error) const;

    /// Destroy behind a noexcept firewall. A broken third-party destroy
    /// entry point must never escape through DriverInstance's destructor.
    void destroy(yaddnsc_driver* driver) const noexcept;

    [[nodiscard]] yaddnsc_status update(yaddnsc_driver* driver, const yaddnsc_update_request& request,
                                        yaddnsc_error& out_error) const;

    /// Whether the plugin exports the OPTIONAL yaddnsc_driver_validate entry
    /// (optional since ABI 1.0). A plugin that does not export it still
    /// loads. `config test` fails, because the host cannot confirm
    /// driver_params.
    [[nodiscard]] bool supports_validate() const noexcept { return validate_ != nullptr; }

    /// Validate a driver_params JSON against the plugin's schema, behind the
    /// same exception firewall as the other trampolines. When the plugin does
    /// not export the optional entry this fails closed with
    /// YADDNSC_STATUS_INVALID_CONFIG: the host cannot confirm driver_params
    /// without the entry, so the absence is a validation failure, not a pass.
    /// Callers that want a friendlier message gate on supports_validate()
    /// first (DriverGateway::validate_config does).
    [[nodiscard]] yaddnsc_status validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                          yaddnsc_error& out_error) const;

private:
    PluginModule() = default;

    /// Report a firewall-caught exception through the ABI error struct,
    /// honouring the caller-supplied struct_size.  The bounded thread-local
    /// storage makes this noexcept path allocation-free: a plugin exception
    /// must never turn into a second termination while reporting it.
    static void write_entry_error(yaddnsc_error& out_error, std::string_view message,
                                  yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR) noexcept;

    SharedLibrary library_;
    decltype(&yaddnsc_driver_get_descriptor) get_descriptor_ = nullptr;
    decltype(&yaddnsc_driver_create) create_ = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy_ = nullptr;
    decltype(&yaddnsc_driver_update) update_ = nullptr;
    decltype(&yaddnsc_driver_validate) validate_ = nullptr;
    DriverDescriptor descriptor_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
