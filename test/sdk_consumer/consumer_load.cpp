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

void log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {}

int32_t cancelled(void*) {
    return 0;
}
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
    if (!descriptor_fn || !create || !destroy || !update)
        return 3;
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    if (descriptor_fn(&descriptor) != YADDNSC_STATUS_OK || !descriptor || descriptor->magic != YADDNSC_DRIVER_MAGIC ||
        !yaddnsc_abi_provides(1, 0, descriptor->abi_major, descriptor->abi_minor))
        return 4;
    int context = 0;
    const yaddnsc_host_services services{sizeof(yaddnsc_host_services), 1, 0, &context, &log, &exchange, &cancelled};
    yaddnsc_driver* driver = nullptr;
    yaddnsc_error error{};
    error.struct_size = sizeof(error);
    if (create(&services, &driver, &error) != YADDNSC_STATUS_OK || !driver)
        return 5;
    yaddnsc_update_request request{};
    request.struct_size = sizeof(request);
    constexpr std::string_view json = "{}";
    request.driver_param_json = {reinterpret_cast<const uint8_t*>(json.data()), json.size()};
    const auto status = update(driver, &request, &error);
    const auto validation = validate ? validate(driver, {json.data(), json.size()}, &error) : YADDNSC_STATUS_OK;
    destroy(driver);
    return status == YADDNSC_STATUS_OK && validation == YADDNSC_STATUS_OK ? 0 : 6;
}
