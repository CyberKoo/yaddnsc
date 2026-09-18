#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define EXAMPLE_EXPORT __declspec(dllexport)
#else
#define EXAMPLE_EXPORT __attribute__((visibility("default")))
#endif

namespace {

constexpr yaddnsc_driver_descriptor descriptor{
        .struct_size = sizeof(yaddnsc_driver_descriptor),
        .api_revision = YADDNSC_DRIVER_API_REVISION,
        .magic = YADDNSC_DRIVER_MAGIC,
        .name = {"consumer_c_abi", sizeof("consumer_c_abi") - 1},
        .version = {"1", sizeof("1") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"installed v1 alpha C ABI consumer", sizeof("installed v1 alpha C ABI consumer") - 1},
        .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

int driver_instance = 0;

} // namespace

extern "C" EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(
        const yaddnsc_driver_descriptor **out_descriptor) {
    if (out_descriptor == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_descriptor = &descriptor;
    return YADDNSC_STATUS_OK;
}

extern "C" EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services *services,
                                                                 yaddnsc_driver **out_driver,
                                                                 yaddnsc_error *out_error) {
    (void) out_error;
    if (services == nullptr || out_driver == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = reinterpret_cast<yaddnsc_driver *>(&driver_instance); // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" EXAMPLE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver *driver) {
    (void) driver;
}

extern "C" EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver *driver,
                                                                 const yaddnsc_update_request *request,
                                                                 yaddnsc_error *out_error) {
    (void) out_error;
    if (driver == nullptr || request == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    return YADDNSC_STATUS_OK;
}
