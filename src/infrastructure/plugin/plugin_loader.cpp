//
// Created by Kotarou on 2026/9/17.
//

#include "plugin_loader.h"

#include "support/fmt.hpp"

#include <spdlog/spdlog.h>

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
    [[nodiscard]] Signature *resolve_entry(const SharedLibrary &library, const char *name) {
        auto *symbol = reinterpret_cast<Signature *>(library.resolve(name)); // NOLINT
        if (symbol == nullptr) {
            SPDLOG_TRACE("Failed to resolve symbol '{}' in '{}'", name, library.path());
        }
        return symbol;
    }

    [[nodiscard]] std::string_view to_view(yaddnsc_string value) noexcept {
        return {value.data, value.size};
    }
} // anonymous namespace

std::expected<PluginModule, domain::PluginError> PluginModule::load(const std::string &path) {
    // 1. dlopen — RTLD_NOW | RTLD_LOCAL (inside SharedLibrary::open).
    auto library = SharedLibrary::open(path);
    if (!library) {
        return std::unexpected(
                make_error(domain::PluginError::Code::LOAD_FAILED, fmt::format("Failed to load driver '{}': {}", path, library.error())));
    }

    PluginModule module;
    module.library_ = std::move(*library);

    // 2. Resolve all required entry points.
    module.get_descriptor_ = resolve_entry<decltype(yaddnsc_driver_get_descriptor)>(module.library_, "yaddnsc_driver_get_descriptor");
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
    const yaddnsc_driver_descriptor *raw_descriptor = nullptr;
    if (const yaddnsc_status status = module.get_descriptor_(&raw_descriptor);
        status != YADDNSC_STATUS_OK || raw_descriptor == nullptr) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' get_descriptor() failed (status {})", path, status)));
    }

    // 4–6. magic, exact api_revision, minimum struct_size.
    if (raw_descriptor->struct_size < YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE) {
        return std::unexpected(make_error(domain::PluginError::Code::ABI_MISMATCH,
                                          fmt::format("Driver '{}' descriptor struct_size {} is below the required "
                                                      "minimum {}. {}",
                                                      path, raw_descriptor->struct_size, YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE,
                                                      ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->magic != YADDNSC_DRIVER_MAGIC) {
        return std::unexpected(make_error(domain::PluginError::Code::ABI_MISMATCH,
                                          fmt::format("Driver '{}' is not a valid yaddnsc driver (magic mismatch)", path)));
    }
    if (raw_descriptor->api_revision != YADDNSC_DRIVER_API_REVISION) {
        return std::unexpected(make_error(domain::PluginError::Code::ABI_MISMATCH,
                                          fmt::format("Driver '{}' reports api_revision {}, host requires {}. {}",
                                                      path, raw_descriptor->api_revision, YADDNSC_DRIVER_API_REVISION,
                                                      ABI_CHANGED_HINT)));
    }
    if (raw_descriptor->name.data == nullptr || raw_descriptor->name.size == 0) {
        return std::unexpected(make_error(domain::PluginError::Code::CONTRACT_VIOLATION,
                                          fmt::format("Driver '{}' reports an empty driver name", path)));
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
