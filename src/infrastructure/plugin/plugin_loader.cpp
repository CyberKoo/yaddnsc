#include "plugin_loader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>  // IWYU pragma: keep — std::exception is caught below
#include <expected>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/plugin/async/cleanup.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/shared_library.h"
#include "support/fmt.hpp"

struct yaddnsc_driver;

namespace {
/// Remedy hints for load-time rejection, picked by which side of the version
/// handshake failed: a different major means the plugin is not built for
/// this ABI at all; a newer minor means the host is older than the plugin
/// requires.
constexpr std::string_view ABI_INCOMPATIBLE_HINT =
    "The plugin is incompatible with this host; use a driver built for ABI v1.";
constexpr std::string_view HOST_TOO_OLD_HINT = "The plugin requires a newer host; upgrade yaddnsc.";

[[nodiscard]] plugin::PluginError make_error(plugin::PluginError::Code code, std::string message) {
    return plugin::PluginError{code, std::move(message)};
}

// Owns no plugin pointers: destroy may overwrite instance or thread-local diagnostics.
struct CreateErrorSnapshot {
    std::array<char, 512> message{};
    std::size_t size = 0;
    uint32_t retry = 0;
    bool valid = false;

    CreateErrorSnapshot(yaddnsc_status status, const yaddnsc_error& error, uint32_t capacity) noexcept {
        valid = yaddnsc_status_is_valid(status) && capacity >= YADDNSC_ERROR_MIN_SIZE &&
                error.struct_size >= YADDNSC_ERROR_MIN_SIZE && error.struct_size <= capacity &&
                error.status == status && yaddnsc_string_is_valid(error.message);
        if (valid) {
            retry = error.retry_after_seconds;
            size = std::min(error.message.size, message.size() - 1);
            if (size != 0) {
                std::memcpy(message.data(), error.message.data, size);
            }
        }
    }
};

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

    // 2. Resolve the common descriptor entry and both candidate groups.
    module.get_descriptor_ =
        resolve_entry<decltype(yaddnsc_driver_get_descriptor)>(module.library_, "yaddnsc_driver_get_descriptor");

    SyncEntryPoints sync;
    sync.create = resolve_entry<decltype(yaddnsc_driver_create)>(module.library_, "yaddnsc_driver_create");
    sync.destroy = resolve_entry<decltype(yaddnsc_driver_destroy)>(module.library_, "yaddnsc_driver_destroy");
    sync.update = resolve_entry<decltype(yaddnsc_driver_update)>(module.library_, "yaddnsc_driver_update");
    sync.validate = resolve_entry<decltype(yaddnsc_driver_validate)>(module.library_, "yaddnsc_driver_validate");

    AsyncEntryPoints async;
    async.create = resolve_entry<decltype(yaddnsc_driver_async_create)>(module.library_, "yaddnsc_driver_async_create");
    async.destroy =
        resolve_entry<decltype(yaddnsc_driver_async_destroy)>(module.library_, "yaddnsc_driver_async_destroy");
    async.begin = resolve_entry<decltype(yaddnsc_driver_update_async_begin)>(module.library_,
                                                                             "yaddnsc_driver_update_async_begin");
    async.resume = resolve_entry<decltype(yaddnsc_driver_update_async_resume)>(module.library_,
                                                                               "yaddnsc_driver_update_async_resume");
    async.validate =
        resolve_entry<decltype(yaddnsc_driver_async_validate)>(module.library_, "yaddnsc_driver_async_validate");

    const bool sync_complete = sync.create != nullptr && sync.destroy != nullptr && sync.update != nullptr;
    const bool any_sync =
        sync.create != nullptr || sync.destroy != nullptr || sync.update != nullptr || sync.validate != nullptr;
    const bool async_complete =
        async.create != nullptr && async.destroy != nullptr && async.begin != nullptr && async.resume != nullptr;
    const bool any_async = async.create != nullptr || async.destroy != nullptr || async.begin != nullptr ||
                           async.resume != nullptr || async.validate != nullptr;

    // Keep the old missing-symbol path for a module with no usable group.
    if (module.get_descriptor_ == nullptr || (!sync_complete && !any_async)) {
        return std::unexpected(make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                                          fmt::format("Driver '{}' does not export the required v1 entry "
                                                      "points (get_descriptor/create/destroy/update). {}",
                                                      path, ABI_INCOMPATIBLE_HINT)));
    }

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

    // 8. Select exactly one execution group. The descriptor's minimum minor
    // is a compatibility requirement, not a mode selector.
    if (sync_complete) {
        if (module.descriptor_.abi_minor == 0) {
            // Legacy exception: the old sync path ignores extra async names.
            module.entries_ = std::move(sync);
            return module;
        }
        if (any_async) {
            return std::unexpected(make_error(
                plugin::PluginError::Code::CONTRACT_VIOLATION,
                fmt::format("Driver '{}' mixes sync and async execution entry points; a module must choose one "
                            "mode. {}",
                            path, ABI_INCOMPATIBLE_HINT)));
        }
        module.entries_ = std::move(sync);
        return module;
    }

    if (any_sync) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::CONTRACT_VIOLATION,
            fmt::format("Driver '{}' mixes sync and async execution entry points; a module must choose one mode. {}",
                        path, ABI_INCOMPATIBLE_HINT)));
    }
    if (module.descriptor_.abi_minor < 1) {
        return std::unexpected(make_error(
            plugin::PluginError::Code::ABI_MISMATCH,
            fmt::format("Driver '{}' exports an async execution group but requires only ABI 1.{}; async mode "
                        "requires at least ABI 1.1. {}",
                        path, module.descriptor_.abi_minor, ABI_INCOMPATIBLE_HINT)));
    }
    if (!async_complete) {
        return std::unexpected(
            make_error(plugin::PluginError::Code::MISSING_SYMBOL,
                       fmt::format("Driver '{}' does not export a complete async execution group "
                                   "(async_create/async_destroy/update_async_begin/update_async_resume). {}",
                                   path, ABI_INCOMPATIBLE_HINT)));
    }
    module.entries_ = std::move(async);
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

    const auto* entries = std::get_if<SyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->create == nullptr) {
        write_entry_error(out_error, "driver is not loaded in sync mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    yaddnsc_status status;
    try {
        status = entries->create(&services, out_driver, &out_error);
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
    const auto* entries = std::get_if<SyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->destroy == nullptr) {
        try {
            SPDLOG_ERROR("Driver '{}' is not loaded in sync mode; sync destroy was not called", path());
        } catch (...) {
        }
        return;
    }
    try {
        entries->destroy(driver);
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
    const auto* entries = std::get_if<SyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->update == nullptr) {
        write_entry_error(out_error, "driver is not loaded in sync mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries->update(driver, &request, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin update");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status PluginModule::validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                      yaddnsc_error& out_error) const {
    const auto* entries = std::get_if<SyncEntryPoints>(&entries_);
    if (entries == nullptr) {
        write_entry_error(out_error, "driver is not loaded in sync mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    // Fail closed: without the entry the host cannot confirm driver_params,
    // so the absence is a validation failure, never a silent pass.
    if (entries->validate == nullptr) {
        write_entry_error(out_error, "plugin does not export yaddnsc_driver_validate; driver_params was not checked",
                          YADDNSC_STATUS_INVALID_CONFIG);
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    try {
        return entries->validate(driver, driver_param_json, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin validate");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status PluginModule::async_create(const yaddnsc_async_services& services, yaddnsc_async_driver** out_driver,
                                          yaddnsc_error& out_error) const {
    if (out_driver == nullptr) {
        write_entry_error(out_error, "out_driver must not be null", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;

    const auto* entries = std::get_if<AsyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->create == nullptr) {
        write_entry_error(out_error, "driver is not loaded in async mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    const uint32_t error_capacity = out_error.struct_size;
    yaddnsc_status status;
    try {
        status = entries->create(&services, out_driver, &out_error);
    } catch (const std::exception& e) {
        out_error.struct_size = error_capacity;
        write_entry_error(out_error, e.what());
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        out_error.struct_size = error_capacity;
        write_entry_error(out_error, "unknown exception from plugin async_create");
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    }

    if (status != YADDNSC_STATUS_OK) {
        if (*out_driver != nullptr) {
            const auto cleanup = [this](yaddnsc_async_driver* driver) noexcept { async_destroy(driver); };
            std::unique_ptr<yaddnsc_async_driver, decltype(cleanup)> failed_driver(std::exchange(*out_driver, nullptr),
                                                                                   cleanup);
            const CreateErrorSnapshot snapshot(status, out_error, error_capacity);
            failed_driver.reset();  // Mandatory cleanup precedes best-effort diagnostics.
            try {
                SPDLOG_WARN(
                    "Driver '{}' ({}) violated the async create contract: failure after storing a handle; destroyed it",
                    descriptor_.name, path());
            } catch (...) {
                // Diagnostics must not change the failed create result.
            }

            if (snapshot.valid) {
                out_error.struct_size = error_capacity;
                write_entry_error(out_error, {snapshot.message.data(), snapshot.size}, status);
                out_error.retry_after_seconds = snapshot.retry;
            } else {
                // Keep malformed reports visibly invalid to the gateway, without
                // retaining a message pointer into the destroyed instance.
                // This trampoline receives a full host-owned C++ object even
                // when its advertised ABI capacity is short.
                out_error = {};
            }
        }
        return status;
    }
    if (*out_driver == nullptr) {
        write_entry_error(out_error, "async_create returned OK with a null handle");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    return status;
}

void PluginModule::async_destroy(yaddnsc_async_driver* driver) const noexcept {
    if (driver == nullptr) {
        return;
    }
    const auto* entries = std::get_if<AsyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->destroy == nullptr) {
        plugin::fatal_async_cleanup_failure(path(), "module is not loaded with an async destroy entry");
    }
    try {
        entries->destroy(driver);
    } catch (const std::exception& e) {
        plugin::fatal_async_cleanup_failure(path(), e.what());
    } catch (...) {
        plugin::fatal_async_cleanup_failure(path(), "destroy threw a non-standard exception");
    }
}

yaddnsc_status PluginModule::async_begin(yaddnsc_async_driver* driver, const yaddnsc_update_request& request,
                                         yaddnsc_error& out_error) const {
    const auto* entries = std::get_if<AsyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->begin == nullptr) {
        write_entry_error(out_error, "driver is not loaded in async mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries->begin(driver, &request, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin async begin");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status PluginModule::async_resume(yaddnsc_async_driver* driver, const yaddnsc_event& event,
                                          yaddnsc_error& out_error) const {
    const auto* entries = std::get_if<AsyncEntryPoints>(&entries_);
    if (entries == nullptr || entries->resume == nullptr) {
        write_entry_error(out_error, "driver is not loaded in async mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries->resume(driver, &event, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin async resume");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status PluginModule::async_validate(yaddnsc_async_driver* driver, yaddnsc_string driver_param_json,
                                            yaddnsc_error& out_error) const {
    const auto* entries = std::get_if<AsyncEntryPoints>(&entries_);
    if (entries == nullptr) {
        write_entry_error(out_error, "driver is not loaded in async mode", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (entries->validate == nullptr) {
        write_entry_error(out_error,
                          "plugin does not export yaddnsc_driver_async_validate; driver_params was not checked",
                          YADDNSC_STATUS_INVALID_CONFIG);
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    try {
        return entries->validate(driver, driver_param_json, &out_error);
    } catch (const std::exception& e) {
        write_entry_error(out_error, e.what());
    } catch (...) {
        write_entry_error(out_error, "unknown exception from plugin async validate");
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
