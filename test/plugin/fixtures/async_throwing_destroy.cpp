/// Loader fixture: a complete async module whose destroy entry throws across
/// the C ABI. The host's async cleanup policy must be fatal because it can no
/// longer prove the instance was released.

#include <stdexcept>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR{
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"async_throwing_destroy", sizeof("async_throwing_destroy") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"Kotarou", sizeof("Kotarou") - 1},
    .description = {"Async destroy throws", sizeof("Async destroy throws") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

int instance_token = 0;

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
                                                                     yaddnsc_error*) {
    if (out_driver == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = reinterpret_cast<yaddnsc_async_driver*>(&instance_token);  // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver*) {
    throw std::runtime_error("async destroy failure");
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(yaddnsc_async_driver*,
                                                                           const yaddnsc_update_request*,
                                                                           yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(yaddnsc_async_driver*, const yaddnsc_event*,
                                                                            yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}
