//
// Created by Kotarou on 2026/9/18.
//

/// Loader fixture: a plugin whose create() violates the handle-ownership
/// contract — it stores a handle in *out_driver and THEN reports failure
/// (status mode) or throws across the ABI (exception mode). The host's
/// create trampoline must destroy and clear the leaked handle before
/// propagating the failure, so no plugin state escapes the failure path.
///
/// Control exports (not part of the driver ABI):
///   leaky_create_set_mode(int) — 0: fail with a status, 1: throw
///   leaky_create_get_state(uint64_t *destroys, uintptr_t *last_destroyed)
///   leaky_create_token()       — the fake handle create() will store

#include <cstdint>
#include <stdexcept>

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
        .name = {"leaky_create", sizeof("leaky_create") - 1},
        .version = {"0.0.0", sizeof("0.0.0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"Fixture whose create stores a handle before failing", sizeof("Fixture whose create stores a handle before failing") - 1},
        .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

constexpr char CREATE_FAILURE_MESSAGE[] = "create failed after storing a handle";

int g_mode = 0;
int g_token = 0;  // stands in for per-instance plugin state
uint64_t g_destroys = 0;
uintptr_t g_last_destroyed = 0;
} // namespace

extern "C" FIXTURE_EXPORT void leaky_create_set_mode(int mode) {
    g_mode = mode;
}

extern "C" FIXTURE_EXPORT void leaky_create_get_state(uint64_t *destroys, uintptr_t *last_destroyed) {
    *destroys = g_destroys;
    *last_destroyed = g_last_destroyed;
}

extern "C" FIXTURE_EXPORT uintptr_t leaky_create_token() {
    return reinterpret_cast<uintptr_t>(&g_token);
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor **out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services * /*services*/,
                                                               yaddnsc_driver **out_driver,
                                                               yaddnsc_error *out_error) {
    // The contract violation: state is handed out first, failure comes after.
    *out_driver = reinterpret_cast<yaddnsc_driver *>(&g_token);
    if (out_error != nullptr && out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        out_error->status = YADDNSC_STATUS_INTERNAL_ERROR;
        out_error->retry_after_seconds = 0;
        out_error->message = {CREATE_FAILURE_MESSAGE, sizeof(CREATE_FAILURE_MESSAGE) - 1};
    }
    if (g_mode == 1) {
        throw std::runtime_error("create exploded after storing a handle");
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver *driver) {
    ++g_destroys;
    g_last_destroyed = reinterpret_cast<uintptr_t>(driver);
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver * /*driver*/,
                                                               const yaddnsc_update_request * /*request*/,
                                                               yaddnsc_error * /*out_error*/) {
    return YADDNSC_STATUS_OK;
}
