#include "plugin_loader.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

#include "domain/dns/record_kind.h"
#include "domain/driver/capabilities.h"
#include "infrastructure/plugin/async/package.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/plugin_package.h"
#include "infrastructure/plugin/shared_library.h"
#include "infrastructure/plugin/sync/package.h"
#include "support/fmt.hpp"

namespace {
constexpr std::string_view ABI_INCOMPATIBLE_HINT =
    "The plugin is incompatible with this host; use a driver built for ABI v1.";
constexpr std::string_view HOST_TOO_OLD_HINT = "The plugin requires a newer host; upgrade yaddnsc.";

/// Static diagnostic for a plugin exception whose detailed text cannot be
/// built — either because the plugin itself ran out of memory or because the
/// host failed while formatting the text.
constexpr std::string_view DESCRIPTOR_EXCEPTION_FALLBACK =
    "Driver get_descriptor() threw an exception; the plugin violated the ABI contract";

[[nodiscard]] plugin::PluginError make_error(const plugin::PluginError::Code code, std::string message) {
    return plugin::PluginError{code, std::move(message)};
}

/// Builds the contract-violation error for an exception that escaped the
/// plugin's get_descriptor() entry point.
///
/// The helper lets no exception escape: a failure while formatting the
/// detailed text (an allocation failure) is caught here and answered with the
/// static fallback, so a second exception cannot cross the loader boundary.
///
/// @param detail  The exception's what() text, or nullptr for an exception
///                that does not derive from std::exception.
[[nodiscard]] plugin::PluginError make_descriptor_error(const std::string& path, const char* detail) noexcept {
    try {
        if (detail != nullptr) {
            return plugin::PluginError{plugin::PluginError::Code::CONTRACT_VIOLATION,
                                       fmt::format("Driver '{}' get_descriptor() threw: {}", path, detail)};
        }
        return plugin::PluginError{plugin::PluginError::Code::CONTRACT_VIOLATION,
                                   fmt::format("Driver '{}' get_descriptor() threw an unknown exception", path)};
    } catch (...) {
        return plugin::make_fallback_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                                           DESCRIPTOR_EXCEPTION_FALLBACK);
    }
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

[[nodiscard]] domain::DriverCapabilities make_capabilities(const std::uint64_t bits) {
    std::vector<domain::RecordKind> records;
    if ((bits & YADDNSC_DRIVER_CAPABILITY_A) != 0) {
        records.push_back(domain::RecordKind::A);
    }
    if ((bits & YADDNSC_DRIVER_CAPABILITY_AAAA) != 0) {
        records.push_back(domain::RecordKind::AAAA);
    }
    return domain::DriverCapabilities(std::move(records));
}

}  // namespace

std::expected<std::shared_ptr<const plugin::PluginPackage>, plugin::PluginError> PluginLoader::load(
    const std::string& path) {
    auto opened = SharedLibrary::open(path);
    if (!opened) {
        return std::unexpected(make_error(plugin::PluginError::Code::LOAD_FAILED,
                                          fmt::format("Failed to load driver '{}': {}", path, opened.error())));
    }
    auto library = std::make_shared<SharedLibrary>(std::move(*opened));

    const auto get_descriptor =
        resolve_entry<decltype(yaddnsc_driver_get_descriptor)>(*library, "yaddnsc_driver_get_descriptor");
    if (get_descriptor == nullptr) {
        return std::unexpected(make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                                          fmt::format("Driver '{}' does not export the required v1 entry "
                                                      "points (get_descriptor/create/destroy/update). {}",
                                                      path, ABI_INCOMPATIBLE_HINT)));
    }

    const yaddnsc_driver_descriptor* raw_descriptor = nullptr;
    yaddnsc_status descriptor_status = YADDNSC_STATUS_INTERNAL_ERROR;
    try {
        descriptor_status = get_descriptor(&raw_descriptor);
    } catch (const std::bad_alloc&) {
        // The plugin ran out of memory: no diagnostic is built, because
        // building one would need the very resource the plugin exhausted.
        return std::unexpected(
            plugin::make_fallback_error(plugin::PluginError::Code::CONTRACT_VIOLATION, DESCRIPTOR_EXCEPTION_FALLBACK));
    } catch (const std::exception& error) {
        return std::unexpected(make_descriptor_error(path, error.what()));
    } catch (...) {
        return std::unexpected(make_descriptor_error(path, nullptr));
    }
    if (descriptor_status != YADDNSC_STATUS_OK || raw_descriptor == nullptr) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                       fmt::format("Driver '{}' get_descriptor() failed (status {})", path, descriptor_status)));
    }

    if (raw_descriptor->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' descriptor struct_size {} does not cover the ABI version prefix {}. {}", path,
                        raw_descriptor->struct_size, YADDNSC_ABI_VERSION_PREFIX_SIZE, ABI_INCOMPATIBLE_HINT)));
    }
    if (raw_descriptor->abi_major != YADDNSC_DRIVER_ABI_MAJOR) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' reports ABI {}.{}, but this host implements ABI {}.x. {}", path,
                                   raw_descriptor->abi_major, raw_descriptor->abi_minor, YADDNSC_DRIVER_ABI_MAJOR,
                                   ABI_INCOMPATIBLE_HINT)));
    }
    if (!yaddnsc_abi_provides(YADDNSC_DRIVER_ABI_MAJOR, YADDNSC_DRIVER_ABI_MINOR, raw_descriptor->abi_major,
                              raw_descriptor->abi_minor)) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::ABI_MISMATCH,
                       fmt::format("Driver '{}' requires ABI {}.{}, but this host provides {}.{}. {}", path,
                                   raw_descriptor->abi_major, raw_descriptor->abi_minor, YADDNSC_DRIVER_ABI_MAJOR,
                                   YADDNSC_DRIVER_ABI_MINOR, HOST_TOO_OLD_HINT)));
    }
    if (raw_descriptor->struct_size < YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' descriptor struct_size {} is below the ABI 1.0 baseline {}. {}", path,
                        raw_descriptor->struct_size, YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE, ABI_INCOMPATIBLE_HINT)));
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

    plugin::PluginDescriptor descriptor{
        .name = std::string(plugin::detail::to_view(raw_descriptor->name)),
        .version = std::string(plugin::detail::to_view(raw_descriptor->version)),
        .author = std::string(plugin::detail::to_view(raw_descriptor->author)),
        .description = std::string(plugin::detail::to_view(raw_descriptor->description)),
        .abi_major = raw_descriptor->abi_major,
        .abi_minor = raw_descriptor->abi_minor,
    };
    auto capabilities = make_capabilities(raw_descriptor->capabilities);

    SyncEntryPoints sync;
    sync.create = resolve_entry<decltype(yaddnsc_driver_create)>(*library, "yaddnsc_driver_create");
    sync.destroy = resolve_entry<decltype(yaddnsc_driver_destroy)>(*library, "yaddnsc_driver_destroy");
    sync.update = resolve_entry<decltype(yaddnsc_driver_update)>(*library, "yaddnsc_driver_update");
    sync.validate = resolve_entry<decltype(yaddnsc_driver_validate)>(*library, "yaddnsc_driver_validate");

    AsyncEntryPoints async;
    async.create = resolve_entry<decltype(yaddnsc_driver_async_create)>(*library, "yaddnsc_driver_async_create");
    async.destroy = resolve_entry<decltype(yaddnsc_driver_async_destroy)>(*library, "yaddnsc_driver_async_destroy");
    async.begin =
        resolve_entry<decltype(yaddnsc_driver_update_async_begin)>(*library, "yaddnsc_driver_update_async_begin");
    async.resume =
        resolve_entry<decltype(yaddnsc_driver_update_async_resume)>(*library, "yaddnsc_driver_update_async_resume");
    async.validate = resolve_entry<decltype(yaddnsc_driver_async_validate)>(*library, "yaddnsc_driver_async_validate");

    const bool sync_complete = sync.create != nullptr && sync.destroy != nullptr && sync.update != nullptr;
    const bool any_sync =
        sync.create != nullptr || sync.destroy != nullptr || sync.update != nullptr || sync.validate != nullptr;
    const bool async_complete =
        async.create != nullptr && async.destroy != nullptr && async.begin != nullptr && async.resume != nullptr;
    const bool any_async = async.create != nullptr || async.destroy != nullptr || async.begin != nullptr ||
                           async.resume != nullptr || async.validate != nullptr;

    if (!sync_complete && !any_async) {
        return std::unexpected(make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                                          fmt::format("Driver '{}' does not export the required v1 entry "
                                                      "points (create/destroy/update or a complete async "
                                                      "execution group). {}",
                                                      path, ABI_INCOMPATIBLE_HINT)));
    }

    if (sync_complete) {
        if (descriptor.abi_minor == 0) {
            return std::make_shared<const plugin::SyncPluginPackage>(library, std::move(descriptor),
                                                                     std::move(capabilities), sync);
        }
        if (any_async) {
            return std::unexpected(make_error(
                plugin::PluginError::Code::CONTRACT_VIOLATION,
                fmt::format("Driver '{}' mixes sync and async execution entry points; a module must choose one "
                            "mode. {}",
                            path, ABI_INCOMPATIBLE_HINT)));
        }
        return std::make_shared<const plugin::SyncPluginPackage>(library, std::move(descriptor),
                                                                 std::move(capabilities), sync);
    }

    if (any_sync) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::CONTRACT_VIOLATION,
                       fmt::format("Driver '{}' mixes sync and async execution entry points; a module must choose one "
                                   "mode. {}",
                                   path, ABI_INCOMPATIBLE_HINT)));
    }
    if (descriptor.abi_minor < 1) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' exports an async execution group but requires only ABI 1.{}; async mode "
                        "requires at least ABI 1.1. {}",
                        path, descriptor.abi_minor, ABI_INCOMPATIBLE_HINT)));
    }
    if (!async_complete) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                       fmt::format("Driver '{}' does not export a complete async execution group "
                                   "(async_create/async_destroy/update_async_begin/update_async_resume). {}",
                                   path, ABI_INCOMPATIBLE_HINT)));
    }
    return std::make_shared<const plugin::AsyncPluginPackage>(library, std::move(descriptor), std::move(capabilities),
                                                              async);
}
