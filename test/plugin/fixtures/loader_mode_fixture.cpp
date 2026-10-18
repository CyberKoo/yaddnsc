/// Loader mode-selection fixture. The CMake target supplies the descriptor
/// minimum minor and which entry groups are exported. The entries are stubs:
/// these tests only exercise load-time symbol grouping, never the calls.

#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

#ifndef YADDNSC_FIXTURE_MINOR
#define YADDNSC_FIXTURE_MINOR 1
#endif
#ifndef YADDNSC_FIXTURE_SYNC
#define YADDNSC_FIXTURE_SYNC 0
#endif
#ifndef YADDNSC_FIXTURE_ASYNC
#define YADDNSC_FIXTURE_ASYNC 0
#endif
#ifndef YADDNSC_FIXTURE_ASYNC_PARTIAL
#define YADDNSC_FIXTURE_ASYNC_PARTIAL 0
#endif
#ifndef YADDNSC_FIXTURE_SYNC_VALIDATE_ONLY
#define YADDNSC_FIXTURE_SYNC_VALIDATE_ONLY 0
#endif

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = static_cast<std::uint16_t>(YADDNSC_FIXTURE_MINOR),
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"loader_mode_fixture", sizeof("loader_mode_fixture") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Loader mode-selection fixture", sizeof("Loader mode-selection fixture") - 1},
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

#if YADDNSC_FIXTURE_SYNC
extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services*, yaddnsc_driver**,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver*, const yaddnsc_update_request*,
                                                               yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
#endif

#if YADDNSC_FIXTURE_SYNC || YADDNSC_FIXTURE_SYNC_VALIDATE_ONLY
extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver*, yaddnsc_string, yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}
#endif

#if YADDNSC_FIXTURE_ASYNC
extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services*,
                                                                     yaddnsc_async_driver**,
                                                                     yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
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

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_validate(yaddnsc_async_driver*, yaddnsc_string,
                                                                       yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}
#elif YADDNSC_FIXTURE_ASYNC_PARTIAL
extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services*,
                                                                     yaddnsc_async_driver**,
                                                                     yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
#endif
