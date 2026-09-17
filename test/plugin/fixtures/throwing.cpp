//
// Created by Kotarou on 2026/9/17.
//

/// Loader fixture: a plugin whose entry points THROW across the C ABI — the
/// shape of a misbehaving third-party plugin. The ABI forbids exceptions, so
/// the host's PluginModule trampolines carry an exception firewall: it must
/// translate every escaping exception into YADDNSC_STATUS_INTERNAL_ERROR with
/// a usable message instead of letting it propagate into the host's frame.
///
/// get_descriptor succeeds so PluginModule::load() accepts the library; the
/// create/update entries throw std::exceptions (covered arm) and validate
/// throws a non-std type (the catch-all arm).

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
        .name = {"throwing", sizeof("throwing") - 1},
        .version = {"0.0.0", sizeof("0.0.0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"Fixture whose entries throw across the ABI", sizeof("Fixture whose entries throw across the ABI") - 1},
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

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services * /*services*/,
                                                               yaddnsc_driver ** /*out_driver*/,
                                                               yaddnsc_error * /*out_error*/) {
    throw std::runtime_error("create exploded");
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver * /*driver*/) {
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver * /*driver*/,
                                                               const yaddnsc_update_request * /*request*/,
                                                               yaddnsc_error * /*out_error*/) {
    throw std::runtime_error("update exploded");
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver * /*driver*/,
                                                                 yaddnsc_string /*driver_param_json*/,
                                                                 yaddnsc_error * /*out_error*/) {
    // A non-std exception type — exercises the catch-all firewall arm.
    throw 7;
}
