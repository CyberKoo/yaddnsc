#include <yaddnsc/sdk/driver_abi.h>

#define EXAMPLE_EXPORT __attribute__((visibility("default")))

static const yaddnsc_driver_descriptor descriptor = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = 0,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"consumer_c_abi", sizeof("consumer_c_abi") - 1},
    .version = {"1", 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"installed C11 ABI consumer", sizeof("installed C11 ABI consumer") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};
static int instance;

EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == NULL)
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    *out = &descriptor;
    return YADDNSC_STATUS_OK;
}

EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* services, yaddnsc_driver** out,
                                                    yaddnsc_error* error) {
    (void) error;
    if (out == NULL)
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    if (services == NULL || services->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
        !yaddnsc_abi_provides(services->abi_major, services->abi_minor, YADDNSC_DRIVER_ABI_MAJOR, 0) ||
        services->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE)
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    *out = (yaddnsc_driver*) &instance;
    return YADDNSC_STATUS_OK;
}

EXAMPLE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* driver) {
    (void) driver;
}

EXAMPLE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* driver, const yaddnsc_update_request* request,
                                                    yaddnsc_error* error) {
    (void) error;
    if (driver == NULL || request == NULL || request->struct_size < YADDNSC_UPDATE_REQUEST_MIN_SIZE)
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    return YADDNSC_STATUS_OK;
}
