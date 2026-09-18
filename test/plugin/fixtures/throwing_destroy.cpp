//
// Created by Kotarou on 2026/9/18.
//

/// Loader fixture: a plugin whose destroy() throws a NON-std type across the
/// C ABI — the shape of a misbehaving third-party plugin hitting the
/// catch-all arm of the host's destroy firewall (the std::exception arm is
/// covered by the `throwing` fixture). The ABI forbids exceptions, so the
/// host's PluginModule::destroy() must swallow the escape: log only, no
/// retry, no terminate, and the module lease is still released afterwards.
///
/// get_descriptor/create/update succeed so PluginModule::load() accepts the
/// library and a full instance cycle reaches destroy().

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
        .name = {"throwing_destroy", sizeof("throwing_destroy") - 1},
        .version = {"0.0.0", sizeof("0.0.0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"Fixture whose destroy throws a non-std type", sizeof("Fixture whose destroy throws a non-std type") - 1},
        .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

// Dummy instance token: create hands out its own address so the host has a
// non-null handle to pass back to update/destroy.
int g_instance_token;
} // namespace

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor **out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services *services,
                                                               yaddnsc_driver **out_driver,
                                                               yaddnsc_error * /*out_error*/) {
    if (services == nullptr || out_driver == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = reinterpret_cast<yaddnsc_driver *>(&g_instance_token); // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver * /*driver*/) {
    // A non-std exception type — exercises the catch-all firewall arm.
    throw 42;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver * /*driver*/,
                                                               const yaddnsc_update_request *request,
                                                               yaddnsc_error * /*out_error*/) {
    if (request == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    return YADDNSC_STATUS_OK;
}
