#include <cstdint>
#include <memory>
#include <string_view>

#include <dlfcn.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/version.hpp>

namespace {
struct DlCloser {
    void operator()(void* library) const noexcept { dlclose(library); }
};

yaddnsc_status exchange(void*, const yaddnsc_http_request*, yaddnsc_http_response* response, yaddnsc_error*) {
    response->status_code = 200;
    response->body = {};
    response->headers = nullptr;
    response->header_count = 0;
    return YADDNSC_STATUS_OK;
}

yaddnsc_status async_http_start(void*, const yaddnsc_http_request*, uint64_t* out_op_id, yaddnsc_error*) {
    if (out_op_id != nullptr) {
        *out_op_id = 1;
    }
    return YADDNSC_STATUS_OK;
}

yaddnsc_status async_http_cancel(void*, uint64_t, yaddnsc_error*) { return YADDNSC_STATUS_OK; }

int32_t async_log_enabled(void*, yaddnsc_log_level) { return 1; }

void log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {}

int32_t cancelled(void*) { return 0; }
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    const std::unique_ptr<void, DlCloser> library(dlopen(argv[1], RTLD_NOW | RTLD_LOCAL));
    if (!library)
        return 2;
    const auto descriptor_fn = reinterpret_cast<decltype(&yaddnsc_driver_get_descriptor)>(
        dlsym(library.get(), "yaddnsc_driver_get_descriptor"));
    const auto create =
        reinterpret_cast<decltype(&yaddnsc_driver_create)>(dlsym(library.get(), "yaddnsc_driver_create"));
    const auto destroy =
        reinterpret_cast<decltype(&yaddnsc_driver_destroy)>(dlsym(library.get(), "yaddnsc_driver_destroy"));
    const auto update =
        reinterpret_cast<decltype(&yaddnsc_driver_update)>(dlsym(library.get(), "yaddnsc_driver_update"));
    const auto validate =
        reinterpret_cast<decltype(&yaddnsc_driver_validate)>(dlsym(library.get(), "yaddnsc_driver_validate"));
    const auto async_create = reinterpret_cast<decltype(&yaddnsc_driver_async_create)>(
        dlsym(library.get(), "yaddnsc_driver_async_create"));
    const auto async_destroy = reinterpret_cast<decltype(&yaddnsc_driver_async_destroy)>(
        dlsym(library.get(), "yaddnsc_driver_async_destroy"));
    const auto async_validate = reinterpret_cast<decltype(&yaddnsc_driver_async_validate)>(
        dlsym(library.get(), "yaddnsc_driver_async_validate"));
    if (!descriptor_fn)
        return 3;
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    if (descriptor_fn(&descriptor) != YADDNSC_STATUS_OK || !descriptor || descriptor->magic != YADDNSC_DRIVER_MAGIC ||
        !yaddnsc_abi_provides(YADDNSC_DRIVER_ABI_MAJOR, YADDNSC_DRIVER_ABI_MINOR, descriptor->abi_major,
                              descriptor->abi_minor))
        return 4;

    constexpr std::string_view json = "{}";
    yaddnsc_error error{};
    error.struct_size = sizeof(error);

    if (create != nullptr && destroy != nullptr && update != nullptr) {
        int context = 0;
        const yaddnsc_host_services services{sizeof(yaddnsc_host_services), 1, 0, &context, &log, &exchange, &cancelled};
        yaddnsc_driver* driver = nullptr;
        if (create(&services, &driver, &error) != YADDNSC_STATUS_OK || !driver)
            return 5;
        yaddnsc_update_request request{};
        request.struct_size = sizeof(request);
        request.driver_param_json = {reinterpret_cast<const uint8_t*>(json.data()), json.size()};
        const auto status = update(driver, &request, &error);
        const auto validation = validate ? validate(driver, {json.data(), json.size()}, &error) : YADDNSC_STATUS_OK;
        destroy(driver);
        return status == YADDNSC_STATUS_OK && validation == YADDNSC_STATUS_OK ? 0 : 6;
    }

    if (async_create != nullptr && async_destroy != nullptr) {
        int context = 0;
        const yaddnsc_async_services services{
            .struct_size = sizeof(yaddnsc_async_services),
            .abi_major = 1,
            .abi_minor = 1,
            .context = &context,
            .log = &log,
            .is_cancelled = &cancelled,
            .http_start = &async_http_start,
            .http_cancel = &async_http_cancel,
            .log_enabled = &async_log_enabled,
        };
        yaddnsc_async_driver* driver = nullptr;
        if (async_create(&services, &driver, &error) != YADDNSC_STATUS_OK || !driver)
            return 5;
        const auto validation =
            async_validate ? async_validate(driver, {json.data(), json.size()}, &error) : YADDNSC_STATUS_OK;
        async_destroy(driver);
        return validation == YADDNSC_STATUS_OK ? 0 : 6;
    }

    return 3;
}
