/// Loader-rejection fixture: ABI major 1, minor 1. The host provides minor 0
/// and must reject a plugin that requires a newer minor.

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = static_cast<uint16_t>(YADDNSC_DRIVER_ABI_MINOR + 1),
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"newer_minor", sizeof("newer_minor") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture that requires a newer ABI minor", sizeof("Fixture that requires a newer ABI minor") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};
}  // namespace

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services*, yaddnsc_driver**,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver*, const yaddnsc_update_request*,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
