#include "plugin_loader.h"

#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#include "infrastructure/plugin/abi_string.h"
#include "infrastructure/plugin/shared_library.h"
#include "support/fmt.hpp"

struct yaddnsc_driver;

namespace {
/// Shared tail of every ABI-mismatch message: points the user at the
/// remedy.
constexpr std::string_view ABI_CHANGED_HINT =
    "The v1 alpha plugin interface has changed, rebuild the driver with the current SDK.";

[[nodiscard]] plugin::PluginError make_error(plugin::PluginError::Code code, std::string message) {
    return plugin::PluginError{code, std::move(message)};
}

template<typename Signature>
    requires std::is_function_v<Signature>
[[nodiscard]] Signature* resolve_entry(const SharedLibrary& library, const char* name) {
    auto* symbol = reinterpret_cast<Signature*>(library.resolve(name));  // NOLINT
    if (symbol == nullptr) {
        SPDLOG_TRACE("Failed to resolve symbol '{}' in '{}'", name, library.path());
    }
    return symbol;
}

}  // anonymous namespace

std::expected<PluginModule, plugin::PluginError> PluginModule::load(const std::string& path) {
    // 1. dlopen — RTLD_NOW | RTLD_LOCAL (inside SharedLibrary::open).
    auto library = SharedLibrary::open(path);
    if (!library) {
        return std::unexpected(make_error(plugin::PluginError::Code::LOAD_FAILED,
                                          fmt::format("Failed to load driver '{}': {}", path, library.error())));
    }

    PluginModule module;
    module.library_ = std::move(*library);

    // 2. Resolve all required entry points.
    module.get_descriptor_ =
        resolve_entry<decltype(yaddnsc_driver_get_descriptor)>(module.library_, "yaddnsc_driver_get_descriptor");
    module.create_ = resolve_entry<decltype(yaddnsc_driver_create)>(module.library_, "yaddnsc_driver_create");
    module.destroy_ = resolve_entry<decltype(yaddnsc_driver_destroy)>(module.library_, "yaddnsc_driver_destroy");
    module.update_ = resolve_entry<decltype(yaddnsc_driver_update)>(module.library_, "yaddnsc_driver_update");
    if (module.get_descriptor_ == nullptr || module.create_ == nullptr || module.destroy_ == nullptr ||
        module.update_ == nullptr) {
        return std::unexpected(make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                                          fmt::format("Driver '{}' does not export the required v1 alpha entry "
                                                      "points (get_descriptor/create/destroy/update). {}",
                                                      path, ABI_CHANGED_HINT)));
    }

    // 2b. The OPTIONAL validate entry (optional since ABI 1.0): absence is
    // not a load error. config test reports that driver_params was not checked.
    module.validate_ = resolve_entry<decltype(yaddnsc_driver_validate)>(module.library_, "yaddnsc_driver_validate");

    // 3. Fetch the descriptor.
    const yaddnsc_driver_descriptor* raw_descriptor = nullptr;
    yaddnsc_status descriptor_status = YADDNSC_STATUS_INTERNAL_ERROR;
    try {
        descriptor_status = module.get_descriptor_(&raw_descriptor);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' get_descriptor() threw: {}", path, e.what())));
    } catch (...) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                       fmt::format("Driver '{}' get_descriptor() threw an unknown exception", path)));
    }
    if (descriptor_status != YADDNSC_STATUS_OK || raw_descriptor == nullptr) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                       fmt::format("Driver '{}' get_descriptor() failed (status {})", path, descriptor_status)));
    }

    // 4–8. version prefix, major, minor, ABI 1.0 baseline, then magic.
    // This host implements major 1 only. A different major is rejected
    // before the 1.0 tail is interpreted.
    if (raw_descriptor->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' descriptor struct_size {} does not cover the ABI version prefix {}. {}", path,
                        raw_descriptor->struct_size, YADDNSC_ABI_VERSION_PREFIX_SIZE, ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->abi_major != YADDNSC_DRIVER_ABI_MAJOR ||
        !yaddnsc_abi_provides(YADDNSC_DRIVER_ABI_MAJOR, YADDNSC_DRIVER_ABI_MINOR, raw_descriptor->abi_major,
                              raw_descriptor->abi_minor)) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' reports ABI {}.{}, host provides {}.{}. {}", path,
                                   raw_descriptor->abi_major, raw_descriptor->abi_minor, YADDNSC_DRIVER_ABI_MAJOR,
                                   YADDNSC_DRIVER_ABI_MINOR, ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->struct_size < YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' descriptor struct_size {} is below the ABI 1.0 baseline {}. {}", path,
                                   raw_descriptor->struct_size, YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE, ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->magic != YADDNSC_DRIVER_MAGIC) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' is not a valid yaddnsc driver (magic mismatch)", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->name) || raw_descriptor->name.size == 0) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' reports an empty driver name", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->version)) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has an invalid descriptor version view", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->author)) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has an invalid descriptor author view", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->description)) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has an invalid descriptor description view", path)));
    }
    if (!yaddnsc_driver_capabilities_are_valid(raw_descriptor->capabilities)) {
        return std::unexpected(make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has unsupported descriptor capabilities", path)));
    }

    // 7. Copy descriptor fields into host-owned storage.
    module.descriptor_ = DriverDescriptor{
        .name = std::string(plugin::detail::to_view(raw_descriptor->name)),
        .version = std::string(plugin::detail::to_view(raw_descriptor->version)),
        .author = std::string(plugin::detail::to_view(raw_descriptor->author)),
        .description = std::string(plugin::detail::to_view(raw_descriptor->description)),
        .capabilities = raw_descriptor->capabilities,
        .abi_major = raw_descriptor->abi_major,
        .abi_minor = raw_descriptor->abi_minor,
    };

    return module;
}

yaddnsc_status PluginModule::create(const yaddnsc_host_services& services, yaddnsc_driver** out_driver,
                                    yaddnsc_error& out_error) const {
    if (out_driver == nullptr) {
        write_entry_error(out_error, "out_driver must not be null", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    // Drop anything the caller left here before the plugin runs, so a plugin
    // that fails without writing cannot make the backstop destroy a handle
    // this call does not own.
    *out_driver = nullptr;

    yaddnsc_status status;
    try {
        status = create_(&services, out_driver, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin create");
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    }

    // Handle-ownership backstop: a failure return must leave *out_driver
    // null. A plugin that stored a handle before failing would otherwise
    // leak it — no DriverInstance exists to own the destroy() call.
    if (status != YADDNSC_STATUS_OK) {
        if (*out_driver != nullptr) {
            SPDLOG_WARN("Driver '{}' ({}) violated the create contract: failure after storing a handle; destroying it",
                        descriptor_.name, path());
            destroy(*out_driver);
            *out_driver = nullptr;
        }
        return status;
    }
    if (*out_driver == nullptr) {
        write_entry_error(out_error, "create returned OK with a null handle");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    return status;
}

void PluginModule::destroy(yaddnsc_driver* driver) const noexcept {
    if (driver == nullptr) {
        return;
    }
    try {
        destroy_(driver);
    } catch (const std::exception& e) {
        try {
            SPDLOG_ERROR("Driver '{}' threw during destroy: {}", path(), e.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            SPDLOG_ERROR("Driver '{}' threw an unknown exception during destroy", path());
        } catch (...) {
        }
    }
}

yaddnsc_status PluginModule::update(yaddnsc_driver* driver, const yaddnsc_update_request& request,
                                    yaddnsc_error& out_error) const {
    try {
        return update_(driver, &request, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin update");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status PluginModule::validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                      yaddnsc_error& out_error) const {
    if (validate_ == nullptr) {
        return YADDNSC_STATUS_OK;
    }
    try {
        return validate_(driver, driver_param_json, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin validate");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

void PluginModule::write_entry_error(yaddnsc_error& out_error, std::string_view message,
                                     yaddnsc_status status) noexcept {
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
    out_error.status = status;
    out_error.retry_after_seconds = 0;
    out_error.message = yaddnsc_string{storage.data(), size};
    out_error.struct_size = out_error.struct_size < static_cast<std::uint32_t>(sizeof(yaddnsc_error))
                                ? out_error.struct_size
                                : static_cast<std::uint32_t>(sizeof(yaddnsc_error));
}
