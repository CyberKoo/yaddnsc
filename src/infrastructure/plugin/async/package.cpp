#include "package.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "infrastructure/plugin/async/cleanup.h"
#include "infrastructure/plugin/detail/entry_error.h"

namespace plugin {

AsyncPluginPackage::AsyncPluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                                       domain::DriverCapabilities capabilities, const AsyncEntryPoints entries)
    : PluginPackage(std::move(library), std::move(descriptor), std::move(capabilities)), entries_(entries) {}

yaddnsc_status AsyncPluginPackage::async_create(const yaddnsc_async_services& services,
                                                yaddnsc_async_driver** out_driver, yaddnsc_error& out_error) const {
    if (out_driver == nullptr) {
        detail::write_entry_error(out_error, "out_driver must not be null", YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (entries_.create == nullptr) {
        detail::write_entry_error(out_error, "driver is not loaded with an async create entry",
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
        detail::write_entry_error(out_error, "unknown exception from plugin async_create");
        status = YADDNSC_STATUS_INTERNAL_ERROR;
    }

    if (status != YADDNSC_STATUS_OK) {
        if (*out_driver != nullptr) {
            const auto cleanup = [this](yaddnsc_async_driver* driver) noexcept { async_destroy(driver); };
            std::unique_ptr<yaddnsc_async_driver, decltype(cleanup)> failed_driver(std::exchange(*out_driver, nullptr),
                                                                                   cleanup);
            const detail::CreateErrorSnapshot snapshot(status, out_error, error_capacity);
            failed_driver.reset();
            try {
                SPDLOG_ERROR(
                    "Driver '{}' ({}) violated the async create contract: failure after storing a handle; destroyed it",
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
        detail::write_entry_error(out_error, "async_create returned OK with a null handle");
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    return status;
}

void AsyncPluginPackage::async_destroy(yaddnsc_async_driver* driver) const noexcept {
    if (driver == nullptr) {
        return;
    }
    if (entries_.destroy == nullptr) {
        fatal_async_cleanup_failure(path(), "module is not loaded with an async destroy entry");
    }
    try {
        entries_.destroy(driver);
    } catch (const std::exception& error) {
        fatal_async_cleanup_failure(path(), error.what());
    } catch (...) {
        fatal_async_cleanup_failure(path(), "destroy threw a non-standard exception");
    }
}

yaddnsc_status AsyncPluginPackage::async_begin(yaddnsc_async_driver* driver, const yaddnsc_update_request& request,
                                               yaddnsc_error& out_error) const {
    if (entries_.begin == nullptr) {
        detail::write_entry_error(out_error, "driver is not loaded with an async begin entry",
                                  YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries_.begin(driver, &request, &out_error);
    } catch (const std::exception& error) {
        detail::write_entry_error(out_error, error.what());
    } catch (...) {
        detail::write_entry_error(out_error, "unknown exception from plugin async begin");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status AsyncPluginPackage::async_resume(yaddnsc_async_driver* driver, const yaddnsc_event& event,
                                                yaddnsc_error& out_error) const {
    if (entries_.resume == nullptr) {
        detail::write_entry_error(out_error, "driver is not loaded with an async resume entry",
                                  YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    try {
        return entries_.resume(driver, &event, &out_error);
    } catch (const std::exception& error) {
        detail::write_entry_error(out_error, error.what());
    } catch (...) {
        detail::write_entry_error(out_error, "unknown exception from plugin async resume");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

yaddnsc_status AsyncPluginPackage::async_validate(yaddnsc_async_driver* driver, const yaddnsc_string driver_param_json,
                                                  yaddnsc_error& out_error) const {
    if (entries_.validate == nullptr) {
        detail::write_entry_error(out_error,
                                  "plugin does not export yaddnsc_driver_async_validate; driver_params was not checked",
                                  YADDNSC_STATUS_INVALID_CONFIG);
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    try {
        return entries_.validate(driver, driver_param_json, &out_error);
    } catch (const std::exception& error) {
        detail::write_entry_error(out_error, error.what());
    } catch (...) {
        detail::write_entry_error(out_error, "unknown exception from plugin async validate");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

}  // namespace plugin
