//
// Created by Kotarou on 2026/9/17.
//

/// Loader-rejection fixture: a loadable shared library that exports only
/// yaddnsc_driver_get_descriptor — create/destroy/update are missing, so the
/// loader must fail at symbol resolution with a MISSING_SYMBOL error.

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
        .magic = YADDNSC_DRIVER_MAGIC,
        .name = {"missing_symbol", sizeof("missing_symbol") - 1},
        .version = {"0.0.0", sizeof("0.0.0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"Fixture missing most entry points", sizeof("Fixture missing most entry points") - 1},
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
