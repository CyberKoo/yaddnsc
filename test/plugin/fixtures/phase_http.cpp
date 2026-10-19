#include <atomic>
#include <expected>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace {
std::atomic<yaddnsc_status> destroy_status{YADDNSC_STATUS_OK};

class PhaseDriver final : public yaddnsc::sdk::Driver {
public:
    explicit PhaseDriver(yaddnsc::sdk::Services services) : services_(services) {
        const auto result = probe();
        if (result != YADDNSC_STATUS_INVALID_ARGUMENT)
            throw std::runtime_error("create permitted HTTP");
    }

    ~PhaseDriver() noexcept override {
        try {
            destroy_status.store(probe());
        } catch (...) {
            destroy_status.store(YADDNSC_STATUS_INTERNAL_ERROR);
        }
    }

    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view) const override {
        if (probe() != YADDNSC_STATUS_INVALID_ARGUMENT)
            return std::unexpected(yaddnsc::sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "validate permitted HTTP", 0});
        return {};
    }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext& context) override {
        yaddnsc::sdk::HttpRequest request;
        request.url = std::string(context.request().driver_param_json);
        const auto* services = services_.get();
        yaddnsc_http_request raw{};
        raw.struct_size = sizeof(raw);
        raw.method = YADDNSC_HTTP_GET;
        raw.url = {request.url.data(), request.url.size()};
        yaddnsc_http_response response{};
        response.struct_size = sizeof(response);
        yaddnsc_error undersized{};
        undersized.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
        undersized.status = YADDNSC_STATUS_RATE_LIMITED;
        for (auto* error : {static_cast<yaddnsc_error*>(nullptr), &undersized}) {
            if (services->http_exchange(services->context, &raw, &response, error) != YADDNSC_STATUS_OK) {
                return std::unexpected(
                    yaddnsc::sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "optional error buffer blocked exchange", 0});
            }
        }
        if (undersized.status != YADDNSC_STATUS_RATE_LIMITED || undersized.struct_size != YADDNSC_ERROR_MIN_SIZE - 1) {
            return std::unexpected(
                yaddnsc::sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "undersized error buffer was written", 0});
        }
        const auto result = services_.exchange(request);
        if (!result)
            return std::unexpected(yaddnsc::sdk::Error{result.error().status, result.error().message, 0});
        return {};
    }

private:
    yaddnsc::sdk::Services services_;

    [[nodiscard]] yaddnsc_status probe() const {
        yaddnsc::sdk::HttpRequest request;
        request.url = "http://127.0.0.1:1/phase-probe";
        const auto result = services_.exchange(request);
        return result ? YADDNSC_STATUS_OK : result.error().status;
    }
};
}  // namespace

YADDNSC_DEFINE_DRIVER(PhaseDriver, "phase_http", "HTTP lifecycle phase fixture", "yaddnsc", "1",
                      YADDNSC_DRIVER_CAPABILITY_A, 0)

extern "C" YADDNSC_SDK_EXPORT yaddnsc_status test_destroy_http_status() {
    return destroy_status.load();
}
