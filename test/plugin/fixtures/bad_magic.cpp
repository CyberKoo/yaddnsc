//
// Created by Kotarou on 2026/9/17.
//

/// Loader-rejection fixture: exports all four entry points but the descriptor
/// carries the wrong magic number — the loader must reject it as "not a valid
/// yaddnsc driver" before ever looking at the revision.

#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .api_revision = YADDNSC_DRIVER_API_REVISION,
    .magic = YADDNSC_DRIVER_MAGIC ^ UINT64_C(0xFF),
    .name = {"bad_magic", sizeof("bad_magic") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture with a wrong magic number", sizeof("Fixture with a wrong magic number") - 1},
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

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services*,
                                                               yaddnsc_driver**,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver*,
                                                               const yaddnsc_update_request*,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
