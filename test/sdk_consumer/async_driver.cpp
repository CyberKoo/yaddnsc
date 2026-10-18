#include <expected>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/async_driver.hpp>

namespace sdk = yaddnsc::sdk;

class AsyncHttpDriver final : public sdk::AsyncDriver {
public:
    [[nodiscard]] sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext& context) override {
        sdk::HttpRequest request;
        request.url = "https://example.invalid/health";
        sdk::AsyncExchangeResult response = co_await context.exchange(std::move(request));
        if (!response) {
            co_return std::unexpected(
                sdk::Error{response.error().status, response.error().message, response.error().retry_after_seconds});
        }
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

YADDNSC_DEFINE_ASYNC_DRIVER(AsyncHttpDriver, "consumer_async", "installed C++ async SDK consumer", "yaddnsc", "1",
                            YADDNSC_DRIVER_CAPABILITY_A)
