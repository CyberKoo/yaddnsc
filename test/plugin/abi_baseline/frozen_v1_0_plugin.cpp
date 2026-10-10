/// Frozen ABI 1.0 baseline plugin — a complete raw C ABI driver (no SDK
/// helper layer) that compiles ONLY against the frozen v1.0 header copy in
/// abi_baseline/v1_0/, never the live include/ tree (see CMakeLists.txt).
///
/// It is the cross-version freeze tripwire: the abi_freeze_compat test
/// dlopens this plugin through the production PluginModule and drives a full
/// create → update → destroy plus validate cycle. A host-side change that
/// breaks ABI 1.0 compatibility turns that test red.
///
/// Never retarget this file at the live header. See
/// docs/development.md#plugin-abi-changes.

#include <new>

#include <yaddnsc/sdk/driver_abi.h>

#define FROZEN_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"frozen_v1_0", sizeof("frozen_v1_0") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Frozen ABI 1.0 baseline plugin", sizeof("Frozen ABI 1.0 baseline plugin") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA,
};

struct FrozenInstance {
    yaddnsc_host_services services;
};

yaddnsc_string make_view(const char* text, size_t size) {
    return yaddnsc_string{text, size};
}

void write_error(yaddnsc_error* out_error, yaddnsc_status status, yaddnsc_string message) {
    if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    out_error->status = status;
    out_error->retry_after_seconds = 0;
    out_error->message = message;
    out_error->struct_size = out_error->struct_size < static_cast<uint32_t>(sizeof(yaddnsc_error))
                                 ? out_error->struct_size
                                 : static_cast<uint32_t>(sizeof(yaddnsc_error));
}

}  // namespace

extern "C" FROZEN_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FROZEN_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* services,
                                                              yaddnsc_driver** out_driver, yaddnsc_error* out_error) {
    if (services == nullptr || out_driver == nullptr) {
        constexpr char MESSAGE[] = "services and out_driver must not be null";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;

    // The version handshake from the plugin side: the host provides the
    // services table; require major 1 with a minor at least the one this
    // plugin was compiled against, then the ABI 1.0 baseline and callbacks.
    if (services->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
        !yaddnsc_abi_provides(services->abi_major, services->abi_minor, YADDNSC_DRIVER_ABI_MAJOR,
                              YADDNSC_DRIVER_ABI_MINOR) ||
        services->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE || services->context == nullptr ||
        services->log == nullptr || services->http_exchange == nullptr || services->is_cancelled == nullptr) {
        constexpr char MESSAGE[] = "incompatible host services table";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    auto* instance = new (std::nothrow) FrozenInstance{};
    if (instance == nullptr) {
        constexpr char MESSAGE[] = "out of memory";
        write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    instance->services = *services;
    *out_driver = reinterpret_cast<yaddnsc_driver*>(instance);  // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FROZEN_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* driver) {
    delete reinterpret_cast<FrozenInstance*>(driver);  // NOLINT
}

extern "C" FROZEN_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* driver,
                                                              const yaddnsc_update_request* request,
                                                              yaddnsc_error* out_error) {
    if (driver == nullptr || request == nullptr) {
        constexpr char MESSAGE[] = "driver and request must not be null";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE) {
        constexpr char MESSAGE[] = "update request struct_size too small";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(request->ip_address) || !yaddnsc_string_is_valid(request->record_type) ||
        !yaddnsc_string_is_valid(request->domain) || !yaddnsc_string_is_valid(request->subdomain) ||
        !yaddnsc_string_is_valid(request->fqdn) || !yaddnsc_bytes_is_valid(request->driver_param_json)) {
        constexpr char MESSAGE[] = "update request contains an invalid view";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }

    auto* instance = reinterpret_cast<FrozenInstance*>(driver);  // NOLINT
    const yaddnsc_host_services& services = instance->services;

    // One log line through host services, with a proper source location.
    constexpr char FILE_NAME[] = "frozen_v1_0_plugin.cpp";
    constexpr char FUNCTION[] = "yaddnsc_driver_update";
    constexpr char LOG_MESSAGE[] = "frozen ABI 1.0 plugin updating";
    const yaddnsc_source_location location{make_view(FILE_NAME, sizeof(FILE_NAME) - 1), __LINE__,
                                           make_view(FUNCTION, sizeof(FUNCTION) - 1)};
    services.log(services.context, YADDNSC_LOG_INFO, make_view(LOG_MESSAGE, sizeof(LOG_MESSAGE) - 1), &location);

    if (services.is_cancelled(services.context) != 0) {
        constexpr char MESSAGE[] = "update cancelled";
        write_error(out_error, YADDNSC_STATUS_CANCELLED, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_CANCELLED;
    }

    // One GET exchange through host services; no body.
    constexpr char URL[] = "https://frozen-v1-0.example.com/nic/update";
    yaddnsc_http_request http_request{};
    http_request.struct_size = static_cast<uint32_t>(sizeof(http_request));
    http_request.url = make_view(URL, sizeof(URL) - 1);
    http_request.method = YADDNSC_HTTP_GET;

    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error exchange_error{};
    exchange_error.struct_size = static_cast<uint32_t>(sizeof(exchange_error));

    const yaddnsc_status exchange_status =
        services.http_exchange(services.context, &http_request, &response, &exchange_error);
    if (exchange_status != YADDNSC_STATUS_OK) {
        // The arena-backed error view stays valid until this update returns.
        write_error(out_error, exchange_status, exchange_error.message);
        return exchange_status;
    }
    if (response.status_code < 200 || response.status_code > 299) {
        constexpr char MESSAGE[] = "upstream rejected the update";
        write_error(out_error, YADDNSC_STATUS_UPSTREAM_REJECTED, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_UPSTREAM_REJECTED;
    }
    return YADDNSC_STATUS_OK;
}

extern "C" FROZEN_EXPORT yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver* driver,
                                                                yaddnsc_string driver_param_json,
                                                                yaddnsc_error* out_error) {
    if (driver == nullptr) {
        constexpr char MESSAGE[] = "driver must not be null";
        write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (!yaddnsc_string_is_valid(driver_param_json) || driver_param_json.size == 0) {
        constexpr char MESSAGE[] = "driver_param_json must be a non-empty JSON object";
        write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, make_view(MESSAGE, sizeof(MESSAGE) - 1));
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    return YADDNSC_STATUS_OK;
}
