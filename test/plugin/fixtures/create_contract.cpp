/// Loader fixture for the create() handle contract.
///
/// create_contract_set_mode:
///   0 — return OK and leave *out_driver untouched
///   1 — return INTERNAL_ERROR and leave *out_driver untouched
/// destroy() counts calls so a test can see whether the host destroyed a
/// pointer the plugin did not store.

#include <cstdint>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR = {
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"create_contract", sizeof("create_contract") - 1},
    .version = {"0.0.0", sizeof("0.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Fixture that does not store a create handle",
                    sizeof("Fixture that does not store a create handle") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

constexpr char FAILURE_MESSAGE[] = "create failed without a handle";

int g_mode = 0;
uint64_t g_destroys = 0;
uintptr_t g_last_destroyed = 0;
}  // namespace

extern "C" FIXTURE_EXPORT void create_contract_set_mode(int mode) {
    g_mode = mode;
}

extern "C" FIXTURE_EXPORT void create_contract_get_state(uint64_t* destroys, uintptr_t* last_destroyed) {
    *destroys = g_destroys;
    *last_destroyed = g_last_destroyed;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* /*services*/,
                                                               yaddnsc_driver** /*out_driver*/,
                                                               yaddnsc_error* out_error) {
    if (g_mode == 0) {
        return YADDNSC_STATUS_OK;
    }
    if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        out_error->status = YADDNSC_STATUS_INTERNAL_ERROR;
        out_error->retry_after_seconds = 0;
        out_error->message = {FAILURE_MESSAGE, sizeof(FAILURE_MESSAGE) - 1};
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* driver) {
    ++g_destroys;
    g_last_destroyed = reinterpret_cast<uintptr_t>(driver);
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* /*driver*/,
                                                               const yaddnsc_update_request* /*request*/,
                                                               yaddnsc_error* /*out_error*/) {
    return YADDNSC_STATUS_OK;
}
