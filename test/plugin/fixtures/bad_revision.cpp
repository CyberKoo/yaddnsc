//
// Created by Kotarou on 2026/9/17.
//

/// Loader-rejection fixture: a structurally complete v1 alpha plugin whose
/// descriptor reports an older api_revision the host no longer accepts. The
/// loader must reject it with an ABI mismatch error pointing at a rebuild.

#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
        .struct_size = sizeof(yaddnsc_driver_descriptor),
        .api_revision = UINT32_C(0),
        .magic = YADDNSC_DRIVER_MAGIC,
        .name = {"bad_revision", sizeof("bad_revision") - 1},
        .version = {"0.0.0", sizeof("0.0.0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"Fixture with an unsupported api_revision", sizeof("Fixture with an unsupported api_revision") - 1},
        .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};
} // namespace

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor **out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services *, yaddnsc_driver **,
                                                               yaddnsc_error *) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver *) {
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver *, const yaddnsc_update_request *,
                                                               yaddnsc_error *) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
