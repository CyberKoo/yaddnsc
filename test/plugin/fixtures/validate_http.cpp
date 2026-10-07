//
// Created by Kotarou on 2026/9/30.
//

/// Loader fixture: validate() calls the services table saved at create().
/// The host must reject that exchange without performing I/O.

#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"validate_http", sizeof("validate_http") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture whose validate calls http_exchange",
                    sizeof("Fixture whose validate calls http_exchange") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA,
};

constexpr char URL[] = "http://example.test/validate";

int g_token = 0;
yaddnsc_host_services g_services{};
int g_called = 0;
yaddnsc_status g_exchange_status = YADDNSC_STATUS_OK;
}  // namespace

extern "C" FIXTURE_EXPORT void validate_http_get_state(int* called, yaddnsc_status* exchange_status) {
    *called = g_called;
    *exchange_status = g_exchange_status;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* services,
                                                               yaddnsc_driver** out_driver,
                                                               yaddnsc_error* /*out_error*/) {
    if (services == nullptr || out_driver == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    g_services = *services;
    *out_driver = reinterpret_cast<yaddnsc_driver*>(&g_token);  // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* /*driver*/) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* /*driver*/,
                                                               const yaddnsc_update_request* /*request*/,
                                                               yaddnsc_error* /*out_error*/) {
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver* /*driver*/,
                                                                 yaddnsc_string /*driver_param_json*/,
                                                                 yaddnsc_error* out_error) {
    yaddnsc_http_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.url = {URL, sizeof(URL) - 1};
    request.method = YADDNSC_HTTP_GET;

    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    g_called = 1;
    g_exchange_status = YADDNSC_STATUS_INTERNAL_ERROR;
    if (g_services.http_exchange != nullptr) {
        g_exchange_status = g_services.http_exchange(g_services.context, &request, &response, &error);
    }
    if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        *out_error = error;
        out_error->status = g_exchange_status;
    }
    return g_exchange_status;
}
