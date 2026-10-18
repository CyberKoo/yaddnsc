/// Async fixture whose create fails with a valid error report. The gateway
/// validate owner must still publish the failure and release the caller; a
/// create failure must not skip the result_ready notification.

#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR{
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"async_create_fails", sizeof("async_create_fails") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"Kotarou", sizeof("Kotarou") - 1},
    .description = {"Async create failure fixture", sizeof("Async create failure fixture") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

constexpr yaddnsc_string CREATE_FAILURE{"create rejected", sizeof("create rejected") - 1};

void write_error(yaddnsc_error* out_error, yaddnsc_status status, yaddnsc_string message) noexcept {
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

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(
    const yaddnsc_driver_descriptor** out_descriptor) {
    if (out_descriptor == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_descriptor = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services*,
                                                                     yaddnsc_async_driver** out_driver,
                                                                     yaddnsc_error* out_error) {
    if (out_driver == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, CREATE_FAILURE);
    return YADDNSC_STATUS_INVALID_CONFIG;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(yaddnsc_async_driver*,
                                                                            const yaddnsc_update_request*,
                                                                            yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(yaddnsc_async_driver*,
                                                                             const yaddnsc_event*,
                                                                             yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_validate(yaddnsc_async_driver*,
                                                                       yaddnsc_string,
                                                                       yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}
