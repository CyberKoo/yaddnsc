/// Loader fixture declaring ONLY the AAAA capability bit.
///
/// The host's record-type gate is a translation of the descriptor's
/// capability bits, and both bits are independent: a driver that supports
/// IPv6 lookups but not IPv4 must report AAAA alone, never a default A.
#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"aaaa_only", sizeof("aaaa_only") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture declaring only the AAAA capability",
                    sizeof("Fixture declaring only the AAAA capability") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_AAAA,
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
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver*, const yaddnsc_update_request*,
                                                               yaddnsc_error* out_error) {
    if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        out_error->status = YADDNSC_STATUS_OK;
    }
    return YADDNSC_STATUS_OK;
}
