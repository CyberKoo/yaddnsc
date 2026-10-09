/// Loader-rejection fixture: struct_size covers the version prefix and the
/// descriptor reports ABI 1.0, but the size stops before the ABI 1.0 baseline.
/// The loader must accept the version and then reject the short baseline.

#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = YADDNSC_ABI_VERSION_PREFIX_SIZE,
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"short_descriptor", sizeof("short_descriptor") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture below the ABI 1.0 baseline", sizeof("Fixture below the ABI 1.0 baseline") - 1},
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
