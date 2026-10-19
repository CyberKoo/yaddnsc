/// Async SDK fixture: a real AsyncDriver using the sequential exchange API.
/// It proves the SDK entry macro emits the async group and that the host and
/// SDK complete a begin/resume exchange cycle.

#include <coroutine>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace {

class AsyncSdkDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(
        yaddnsc::sdk::AsyncUpdateContext& context) override {
        yaddnsc::sdk::HttpRequest request;
        request.url = context.request().driver_param_json;
        auto response = co_await context.exchange(std::move(request));
        if (!response) {
            co_return std::unexpected(yaddnsc::sdk::Error{response.error().status, response.error().message,
                                                          response.error().retry_after_seconds});
        }
        co_return {};
    }

    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override {
        if (driver_param_json.empty()) {
            return std::unexpected(
                yaddnsc::sdk::Error{YADDNSC_STATUS_INVALID_CONFIG, "empty driver_params", 0});
        }
        return {};
    }
};

}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(AsyncSdkDriver, "async_sdk_driver", "Async SDK contract fixture", "yaddnsc", "1.0.0",
                            YADDNSC_DRIVER_CAPABILITY_A)
