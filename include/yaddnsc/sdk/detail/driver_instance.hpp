#ifndef YADDNSC_SDK_DETAIL_DRIVER_INSTANCE_HPP
#define YADDNSC_SDK_DETAIL_DRIVER_INSTANCE_HPP

/// Host-side ABI wiring for the synchronous execution group.
///
/// The instance bookkeeping and the four entry helpers below implement the C
/// ABI, not the plugin interface: a driver author writes a `Driver` subclass
/// and expands `YADDNSC_DEFINE_DRIVER`, and never calls anything here.
/// `<yaddnsc/sdk/driver.hpp>` includes this file last so the single-header
/// path keeps working for existing plugin sources.

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <yaddnsc/sdk/driver.hpp>

namespace yaddnsc::sdk::detail {

struct DriverInstance {
    // Driver destruction may log; destroy it before its borrowed services.
    yaddnsc_host_services_v1_1 services{};
    std::string error_storage;
    std::unique_ptr<Driver> driver;
};

template<typename DriverClass, uint16_t RequiredMinor = 0>
inline yaddnsc_status create_driver(const yaddnsc_host_services* services, yaddnsc_driver** out_driver,
                                    yaddnsc_error* out_error) noexcept {
    if (services == nullptr || out_driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "services and out_driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (services->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
        !yaddnsc_abi_provides(services->abi_major, services->abi_minor, YADDNSC_DRIVER_ABI_MAJOR, RequiredMinor) ||
        services->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE || services->context == nullptr ||
        services->log == nullptr || services->http_exchange == nullptr || services->is_cancelled == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "incompatible host services table", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    static_assert(std::is_nothrow_destructible_v<DriverClass>,
                  "driver classes must be nothrow destructible across the C ABI");
    constexpr std::string_view fallback = "unknown exception during driver construction";
    std::string_view reported;
    try {
        auto instance = std::make_unique<DriverInstance>();
        instance->services = copy_services_v1_1(*services);
        if constexpr (std::is_constructible_v<DriverClass, Services>)
            instance->driver =
                std::make_unique<DriverClass>(Services{&instance->services.base, instance->services.log_enabled});
        else
            instance->driver = std::make_unique<DriverClass>();
        *out_driver = reinterpret_cast<yaddnsc_driver*>(instance.release());  // NOLINT
        return YADDNSC_STATUS_OK;
    } catch (const std::exception& e) {
        reported = copy_bounded(e.what(), fallback);
    } catch (...) {
        reported = copy_bounded({}, fallback);
    }
    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, reported, 0);
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

inline void destroy_driver(yaddnsc_driver* driver) noexcept {
    delete reinterpret_cast<DriverInstance*>(driver);  // NOLINT
}

inline yaddnsc_status update_driver(yaddnsc_driver* driver, const yaddnsc_update_request* request,
                                    yaddnsc_error* out_error) noexcept {
    if (driver == nullptr || request == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver and request must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "update request struct_size too small", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_update_request(*request)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "update request contains an invalid view", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    auto* instance = reinterpret_cast<DriverInstance*>(driver);  // NOLINT
    try {
        const UpdateRequest update_request{to_view(request->ip_address), to_view(request->record_type),
                                           to_view(request->domain),     to_view(request->subdomain),
                                           to_view(request->fqdn),       to_view(request->driver_param_json)};
        UpdateContext context{update_request, &instance->services.base, instance->services.log_enabled};
        return translate_result(instance->driver->update(context), instance->error_storage, out_error);
    } catch (const ConfigParseError& e) {
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid driver configuration"),
                    0);
        return YADDNSC_STATUS_INVALID_CONFIG;
    } catch (const std::exception& e) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "unknown exception during update"),
                    0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded({}, "unknown exception during update"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

inline yaddnsc_status validate_driver(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                      yaddnsc_error* out_error) noexcept {
    if (driver == nullptr) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(driver_param_json)) {
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "driver_param_json contains an invalid view", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    auto* instance = reinterpret_cast<DriverInstance*>(driver);  // NOLINT
    try {
        return translate_result(instance->driver->validate(to_view(driver_param_json)), instance->error_storage,
                                out_error);
    } catch (const ConfigParseError& e) {
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid driver configuration"),
                    0);
        return YADDNSC_STATUS_INVALID_CONFIG;
    } catch (const std::exception& e) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                    copy_bounded(e.what(), "unknown exception during validate"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded({}, "unknown exception during validate"), 0);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace yaddnsc::sdk::detail

#endif  // YADDNSC_SDK_DETAIL_DRIVER_INSTANCE_HPP
