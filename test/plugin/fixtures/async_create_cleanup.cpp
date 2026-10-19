
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

struct yaddnsc_async_driver {
    std::array<char, 768> message;
};

namespace {
constexpr yaddnsc_driver_descriptor DESCRIPTOR{
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"async_create_cleanup", sizeof("async_create_cleanup") - 1},
    .version = {"1.0.0", 5},
    .author = {"Test", 4},
    .description = {"Failed create cleanup", sizeof("Failed create cleanup") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

// Controlled only by serial lifecycle tests; these exports are not driver ABI.
int scenario = 0;
uint64_t creates = 0;
uint64_t destroys = 0;
uintptr_t message_address = 0;
}  // namespace

extern "C" FIXTURE_EXPORT void async_create_cleanup_set_scenario(int value) {
    scenario = value;
    creates = destroys = 0;
    message_address = 0;
}

extern "C" FIXTURE_EXPORT void async_create_cleanup_get_state(uint64_t* out_creates, uint64_t* out_destroys,
                                                              uintptr_t* out_message) {
    *out_creates = creates;
    *out_destroys = destroys;
    *out_message = message_address;
}

extern "C" FIXTURE_EXPORT yaddnsc_status
yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out_descriptor) {
    *out_descriptor = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services*,
                                                                     yaddnsc_async_driver** out_driver,
                                                                     yaddnsc_error* error) {
    ++creates;
    auto owner = std::make_unique<yaddnsc_async_driver>();
    owner->message.fill('m');
    message_address = reinterpret_cast<uintptr_t>(owner->message.data());
    yaddnsc_async_driver* driver = owner.release();
    // The handle is published before the throws below: the host owns the
    // driver on every path, including the ones that unwind.
    *out_driver = driver;
    if (scenario == 7) {
        throw std::runtime_error("async create threw after handle");
    }
    if (scenario == 8) {
        throw 42;
    }
    if (error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
        // Scenario 11 deliberately lies about the caller's capacity.
        if (scenario != 11) {
            return YADDNSC_STATUS_RATE_LIMITED;
        }
    }
    error->struct_size = static_cast<uint32_t>(sizeof(*error));
    error->status = YADDNSC_STATUS_RATE_LIMITED;
    error->retry_after_seconds = 73;
    error->message = {driver->message.data(), scenario == 2 ? driver->message.size() : 23};
    switch (scenario) {
        case 1:
            error->message = {nullptr, 0};
            break;
        case 3:
            error->message = {nullptr, 1};
            break;
        case 4:
            error->struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
            error->message = {nullptr, 1};
            break;
        case 5:
            error->status = YADDNSC_STATUS_INVALID_CONFIG;
            break;
        case 6:
            return static_cast<yaddnsc_status>(9999);
        case 9:
            ++error->struct_size;
            break;
        case 12:
            error->status = YADDNSC_STATUS_PENDING;
            return YADDNSC_STATUS_PENDING;
        case 13:
            error->status = static_cast<yaddnsc_status>(9999);
            break;
        default:
            break;
    }
    return YADDNSC_STATUS_RATE_LIMITED;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver) {
    ++destroys;
    if (scenario == 10) {
        throw std::runtime_error("failed create destroy threw");
    }
    // Both overwrite and release: retaining the view fails even without ASan.
    std::unique_ptr<yaddnsc_async_driver> owner{driver};
    owner->message.fill('x');
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(yaddnsc_async_driver*,
                                                                           const yaddnsc_update_request*,
                                                                           yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(yaddnsc_async_driver*, const yaddnsc_event*,
                                                                            yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}
