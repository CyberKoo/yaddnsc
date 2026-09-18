//
// Created by Kotarou on 2026/9/17.
//

#include "plugin_loader.h"

#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/plugin/shared_library.h"
#include "support/fmt.hpp"

struct yaddnsc_driver;

namespace {
/// Shared tail of every ABI-mismatch message: points the user at the
/// remedy.
constexpr std::string_view ABI_CHANGED_HINT =
    "The v1 alpha plugin interface has changed, rebuild the driver with the current SDK.";

[[nodiscard]] domain::PluginError make_error(domain::PluginError::Code code, std::string message) {
    return domain::PluginError{code, std::move(message)};
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

[[nodiscard]] std::string_view to_view(yaddnsc_string value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}
}  // anonymous namespace

std::expected<PluginModule, domain::PluginError> PluginModule::load(const std::string& path) {
    // 1. dlopen — RTLD_NOW | RTLD_LOCAL (inside SharedLibrary::open).
    auto library = SharedLibrary::open(path);
    if (!library) {
        return std::unexpected(make_error(domain::PluginError::Code::LOAD_FAILED,
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
        return std::unexpected(make_error(domain::PluginError::Code::MISSING_SYMBOL,
                                          fmt::format("Driver '{}' does not export the required v1 alpha entry "
                                                      "points (get_descriptor/create/destroy/update). {}",
                                                      path, ABI_CHANGED_HINT)));
    }

    // 2b. The OPTIONAL validate entry (added within api_revision 1): absence
    // is not an error — the host skips the driver-side config check for
    // plugins built against an SDK that predates it.
    module.validate_ = resolve_entry<decltype(yaddnsc_driver_validate)>(module.library_, "yaddnsc_driver_validate");

    // 3. Fetch the descriptor.
    const yaddnsc_driver_descriptor* raw_descriptor = nullptr;
    yaddnsc_status descriptor_status = YADDNSC_STATUS_INTERNAL_ERROR;
    try {
        descriptor_status = module.get_descriptor_(&raw_descriptor);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' get_descriptor() threw: {}", path, e.what())));
    } catch (...) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' get_descriptor() threw an unknown exception", path)));
    }
    if (descriptor_status != YADDNSC_STATUS_OK || raw_descriptor == nullptr) {
        return std::unexpected(
            make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                       fmt::format("Driver '{}' get_descriptor() failed (status {})", path, descriptor_status)));
    }

    // 4–6. magic, exact api_revision, minimum struct_size.
    if (raw_descriptor->struct_size < YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE) {
        return std::unexpected(make_error(
            domain::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' descriptor struct_size {} is below the required "
                        "minimum {}. {}",
                        path, raw_descriptor->struct_size, YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE, ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->magic != YADDNSC_DRIVER_MAGIC) {
        return std::unexpected(
            make_error(domain::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' is not a valid yaddnsc driver (magic mismatch)", path)));
    }
    if (raw_descriptor->api_revision != YADDNSC_DRIVER_API_REVISION) {
        return std::unexpected(
            make_error(domain::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' reports api_revision {}, host requires {}. {}", path,
                                   raw_descriptor->api_revision, YADDNSC_DRIVER_API_REVISION, ABI_CHANGED_HINT)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->name) || raw_descriptor->name.size == 0) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' reports an empty driver name", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->version)) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has an invalid descriptor version view", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->author)) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has an invalid descriptor author view", path)));
    }
    if (!yaddnsc_string_is_valid(raw_descriptor->description)) {
        return std::unexpected(make_error(
            domain::PluginError::Code::CONTRACT_VIOLATION,
            fmt::format("Driver '{}' has an invalid descriptor description view", path)));
    }
    if (!yaddnsc_driver_capabilities_are_valid(raw_descriptor->capabilities)) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' has unsupported descriptor capabilities", path)));
    }

    // 7. Copy descriptor fields into host-owned storage.
    module.descriptor_ = DriverDescriptor{
        .name = std::string(to_view(raw_descriptor->name)),
        .version = std::string(to_view(raw_descriptor->version)),
        .author = std::string(to_view(raw_descriptor->author)),
        .description = std::string(to_view(raw_descriptor->description)),
        .capabilities = raw_descriptor->capabilities,
        .api_revision = raw_descriptor->api_revision,
    };

    return module;
}

yaddnsc_status PluginModule::create(const yaddnsc_host_services& services, yaddnsc_driver** out_driver,
                                    yaddnsc_error& out_error) const {
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
    if (status != YADDNSC_STATUS_OK && out_driver != nullptr && *out_driver != nullptr) {
        SPDLOG_WARN("Driver '{}' ({}) violated the create contract: failure after storing a handle; destroying it",
                    descriptor_.name, path());
        destroy(*out_driver);
        *out_driver = nullptr;
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
