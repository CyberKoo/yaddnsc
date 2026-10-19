/// Loader-rejection fixture: exports only yaddnsc_driver_get_descriptor and
/// declares a descriptor minor one newer than the host. The host resolves no
/// usable execution entry from it — as with a plugin built for a newer minor
/// whose entries it does not know — and must still diagnose the version
/// handshake (host too old), not missing entry points.

#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = static_cast<std::uint16_t>(YADDNSC_DRIVER_ABI_MINOR + 1),
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"newer_minor_no_entries", sizeof("newer_minor_no_entries") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture that requires a newer ABI minor and exports no known entries",
                    sizeof("Fixture that requires a newer ABI minor and exports no known entries") - 1},
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
