#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>

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

/// The execution protocol selected for one loaded module. The mode is fixed
/// at load; it is decided from the exported entry groups and the descriptor,
/// never from abi_minor alone.
enum class ExecutionMode { Sync, Async };

struct SyncEntryPoints {
    decltype(&yaddnsc_driver_create) create = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update) update = nullptr;
    decltype(&yaddnsc_driver_validate) validate = nullptr;
};

struct AsyncEntryPoints {
    decltype(&yaddnsc_driver_async_create) create = nullptr;
    decltype(&yaddnsc_driver_async_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update_async_begin) begin = nullptr;
    decltype(&yaddnsc_driver_update_async_resume) resume = nullptr;
    decltype(&yaddnsc_driver_async_validate) validate = nullptr;
};

using EntryPoints = std::variant<SyncEntryPoints, AsyncEntryPoints>;

/// PluginModule — one loaded driver plugin: the shared library handle, the
/// validated descriptor, and exactly one verified execution entry group.
///
/// Loading follows the ABI protocol order: dlopen(RTLD_NOW|RTLD_LOCAL) →
/// resolve get_descriptor → get_descriptor() → version prefix →
/// abi_major/abi_minor (`yaddnsc_abi_provides`) → minor baseline struct_size →
/// magic, strings, and capability bits → copy descriptor fields into host-owned
/// storage → resolve the execution-group symbols → mode selection, which reads
/// the just-copied abi_minor. The version handshake precedes the group checks,
/// so a plugin that requires a newer minor is reported as a host-version
/// mismatch, never as missing entry points. Only then may the selected group
/// be called. The sync validate and async validate entries are optional; a
/// missing one leaves that mode's validate pointer null.
///
/// Mode selection is fixed at load. A complete sync group with minimum minor 0
/// stays on the synchronous path even when async names are also exported. Async
/// mode requires a complete async group, no sync execution symbol, and
/// minimum minor >= 1. New-protocol modules may not mix groups.
///
/// @note Thread-safe for concurrent calls on the selected entry group (they
///       only read the entry-point table); load is single-threaded startup work.
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

    /// The execution protocol fixed at load.
    [[nodiscard]] ExecutionMode mode() const noexcept {
        return std::holds_alternative<AsyncEntryPoints>(entries_) ? ExecutionMode::Async : ExecutionMode::Sync;
    }

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
    [[nodiscard]] bool supports_validate() const noexcept {
        if (const auto* sync = std::get_if<SyncEntryPoints>(&entries_)) {
            return sync->validate != nullptr;
        }
        return std::get<AsyncEntryPoints>(entries_).validate != nullptr;
    }

    /// Validate a driver_params JSON against the plugin's schema, behind the
    /// same exception firewall as the other trampolines. When the plugin does
    /// not export the optional entry this fails closed with
    /// YADDNSC_STATUS_INVALID_CONFIG: the host cannot confirm driver_params
    /// without the entry, so the absence is a validation failure, not a pass.
    /// Callers that want a friendlier message gate on supports_validate()
    /// first (DriverGateway::validate_config does).
    [[nodiscard]] yaddnsc_status validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                          yaddnsc_error& out_error) const;

    /// Async-group trampolines. The status-returning ones answer
    /// INVALID_ARGUMENT when the module was loaded in sync mode; the host
    /// gateway must branch on mode() first. async_destroy is the exception: it
    /// returns void and calls fatal_async_cleanup_failure (std::abort) if the
    /// module is not in async mode or its destroy entry throws, because a
    /// handle the host cannot prove it released may not be dropped.
    /// Failed async_create handles are destroyed exactly once before returning,
    /// even if diagnostic logging fails; destroy failure is fatal. Valid error
    /// messages are copied before cleanup (at most 511 bytes), preserving empty
    /// messages and retry hints. Malformed reports remain invalid to the caller.
    /// Copied messages use host thread-local storage until the next entry error
    /// on that thread; callers must capture them before another plugin call.
    [[nodiscard]] yaddnsc_status async_create(const yaddnsc_async_services& services, yaddnsc_async_driver** out_driver,
                                              yaddnsc_error& out_error) const;
    void async_destroy(yaddnsc_async_driver* driver) const noexcept;
    [[nodiscard]] yaddnsc_status async_begin(yaddnsc_async_driver* driver, const yaddnsc_update_request& request,
                                             yaddnsc_error& out_error) const;
    [[nodiscard]] yaddnsc_status async_resume(yaddnsc_async_driver* driver, const yaddnsc_event& event,
                                              yaddnsc_error& out_error) const;
    [[nodiscard]] yaddnsc_status async_validate(yaddnsc_async_driver* driver, yaddnsc_string driver_param_json,
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
    EntryPoints entries_{SyncEntryPoints{}};
    DriverDescriptor descriptor_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
