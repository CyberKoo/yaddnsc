#include <yaddnsc/sdk/driver.hpp>

#include <optional>

namespace sdk = yaddnsc::sdk;

class HttpDriver final : public yaddnsc::sdk::Driver {
public:
    [[nodiscard]] sdk::Result update(sdk::UpdateContext &context) override {
        const sdk::HttpRequest request{
                .method = sdk::Method::GET,
                .url = "https://example.invalid/health",
                .headers = {},
                .body = std::nullopt,
                .content_type = {},
        };
        const auto response = context.exchange(request);
        if (!response) {
            return std::unexpected(sdk::Error{response.error().status, response.error().message,
                                              response.error().retry_after_seconds});
        }
        return {};
    }
};

YADDNSC_DEFINE_DRIVER(HttpDriver, "consumer_http", "installed C++ HTTP SDK consumer", "yaddnsc", "1",
                      YADDNSC_DRIVER_CAPABILITY_A)
