//
// Descriptor-view rejection fixture. CMake builds this source once per
// invalid field so PluginModule::load is exercised through a real .so.
//

#include <yaddnsc/sdk/driver_abi.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

namespace {
#if defined(YADDNSC_FIXTURE_INVALID_NAME)
constexpr yaddnsc_string NAME{nullptr, 1};
constexpr yaddnsc_string VERSION{"0.0.0", sizeof("0.0.0") - 1};
constexpr yaddnsc_string AUTHOR{"yaddnsc", sizeof("yaddnsc") - 1};
constexpr yaddnsc_string DESCRIPTION{"invalid name view", sizeof("invalid name view") - 1};
constexpr uint64_t CAPABILITIES = YADDNSC_DRIVER_CAPABILITY_A;
#elif defined(YADDNSC_FIXTURE_INVALID_VERSION)
constexpr yaddnsc_string NAME{"invalid_descriptor", sizeof("invalid_descriptor") - 1};
constexpr yaddnsc_string VERSION{nullptr, 1};
constexpr yaddnsc_string AUTHOR{"yaddnsc", sizeof("yaddnsc") - 1};
constexpr yaddnsc_string DESCRIPTION{"invalid version view", sizeof("invalid version view") - 1};
constexpr uint64_t CAPABILITIES = YADDNSC_DRIVER_CAPABILITY_A;
#elif defined(YADDNSC_FIXTURE_INVALID_AUTHOR)
constexpr yaddnsc_string NAME{"invalid_descriptor", sizeof("invalid_descriptor") - 1};
constexpr yaddnsc_string VERSION{"0.0.0", sizeof("0.0.0") - 1};
constexpr yaddnsc_string AUTHOR{nullptr, 1};
constexpr yaddnsc_string DESCRIPTION{"invalid author view", sizeof("invalid author view") - 1};
constexpr uint64_t CAPABILITIES = YADDNSC_DRIVER_CAPABILITY_A;
#elif defined(YADDNSC_FIXTURE_INVALID_DESCRIPTION)
constexpr yaddnsc_string NAME{"invalid_descriptor", sizeof("invalid_descriptor") - 1};
constexpr yaddnsc_string VERSION{"0.0.0", sizeof("0.0.0") - 1};
constexpr yaddnsc_string AUTHOR{"yaddnsc", sizeof("yaddnsc") - 1};
constexpr yaddnsc_string DESCRIPTION{nullptr, 1};
constexpr uint64_t CAPABILITIES = YADDNSC_DRIVER_CAPABILITY_A;
#elif defined(YADDNSC_FIXTURE_INVALID_CAPABILITIES)
constexpr yaddnsc_string NAME{"invalid_descriptor", sizeof("invalid_descriptor") - 1};
constexpr yaddnsc_string VERSION{"0.0.0", sizeof("0.0.0") - 1};
constexpr yaddnsc_string AUTHOR{"yaddnsc", sizeof("yaddnsc") - 1};
constexpr yaddnsc_string DESCRIPTION{"invalid capabilities", sizeof("invalid capabilities") - 1};
constexpr uint64_t CAPABILITIES = UINT64_C(1) << 63;
#else
#error "select one invalid descriptor fixture field"
#endif

constexpr yaddnsc_driver_descriptor DESCRIPTOR{
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .api_revision = YADDNSC_DRIVER_API_REVISION,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = NAME,
    .version = VERSION,
    .author = AUTHOR,
    .description = DESCRIPTION,
    .capabilities = CAPABILITIES,
};
}  // namespace

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services*,
                                                                  yaddnsc_driver**,
                                                                  yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver*) {}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update(yaddnsc_driver*,
                                                                  const yaddnsc_update_request*,
                                                                  yaddnsc_error*) {
    return YADDNSC_STATUS_INTERNAL_ERROR;
}
