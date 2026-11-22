#include "package.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "infrastructure/plugin/detail/entry_error.h"

namespace plugin {

SyncPluginPackage::SyncPluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                                     domain::DriverCapabilities capabilities, const SyncEntryPoints entries)
    : PluginPackage(std::move(library), std::move(descriptor), std::move(capabilities)), entries_(entries) {}

yaddnsc_status SyncPluginPackage::create(const yaddnsc_host_services& services, yaddnsc_driver** out_driver,
                                         yaddnsc_error& out_error) const {
    if (out_driver == nullptr) {
        detail::write_entry_error(out_error, "out_driver must not be null", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (entries_.create == nullptr) {
        detail::write_entry_error(out_error, "driver is not loaded with a sync create entry",
                                  YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    const std::uint32_t error_capacity = out_error.struct_size;
    yaddnsc_status status;
    try {
        status = entries_.create(&services, out_driver, &out_error);
    } catch (const std::exception& error) {
        out_error.struct_size = error_capacity;
        detail::write_entry_error(out_error, error.what());
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        out_error.struct_size = error_capacity;
        detail::write_entry_error(out_error, "unknown exception from plugin create");
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    }

    if (status != YADDNSC_STATUS_OK) {
        if (*out_driver != nullptr) {
            const auto cleanup = [this](yaddnsc_driver* driver) noexcept { destroy(driver); };
            std::unique_ptr<yaddnsc_driver, decltype(cleanup)> failed_driver(std::exchange(*out_driver, nullptr),
                                                                             cleanup);
            const detail::CreateErrorSnapshot snapshot(status, out_error, error_capacity);
            failed_driver.reset();
            try {
                SPDLOG_ERROR(
                    "Driver '{}' ({}) violated the create contract: failure after storing a handle; destroyed it",
                    descriptor().name, path());
            } catch (...) {
            }

            if (snapshot.valid) {
                out_error.struct_size = error_capacity;
                detail::write_entry_error(out_error, {snapshot.message.data(), snapshot.size}, status);
                out_error.retry_after_seconds = snapshot.retry;
            } else {
                out_error = {};
            }
        }
        return status;
    }
    if (*out_driver == nullptr) {
        detail::write_entry_error(out_error, "create returned OK with a null handle");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    return status;
}

void SyncPluginPackage::destroy(yaddnsc_driver* driver) const noexcept {
    if (driver == nullptr) {
        return;
    }
    if (entries_.destroy == nullptr) {
        try {
            SPDLOG_ERROR("Driver '{}' has no sync destroy entry; sync destroy was not called", path());
        } catch (...) {
        }
        return;
    }
    try {
        entries_.destroy(driver);
    } catch (const std::exception& error) {
        try {
            SPDLOG_ERROR("Driver '{}' threw during destroy: {}", path(), error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            SPDLOG_ERROR("Driver '{}' threw an unknown exception during destroy", path());
        } catch (...) {
        }
    }
}

yaddnsc_status SyncPluginPackage::update(yaddnsc_driver* driver, const yaddnsc_update_request& request,
                                         yaddnsc_error& out_error) const {
    if (entries_.update == nullptr) {
        detail::write_entry_error(out_error, "driver is not loaded with a sync update entry",
                                  YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries_.update(driver, &request, &out_error);
    } catch (const std::exception& error) {
        detail::write_entry_error(out_error, error.what());
    } catch (...) {
        detail::write_entry_error(out_error, "unknown exception from plugin update");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status SyncPluginPackage::validate(yaddnsc_driver* driver, const yaddnsc_string driver_param_json,
                                           yaddnsc_error& out_error) const {
    if (entries_.validate == nullptr) {
        detail::write_entry_error(out_error,
                                  "plugin does not export yaddnsc_driver_validate; driver_params was not checked",
                                  YADDNSC_STATUS_INVALID_CONFIG);
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    try {
        return entries_.validate(driver, driver_param_json, &out_error);
    } catch (const std::exception& error) {
        detail::write_entry_error(out_error, error.what());
    } catch (...) {
        detail::write_entry_error(out_error, "unknown exception from plugin validate");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

}  // namespace plugin
