/// Async SDK fixture: a real AsyncDriver using single and grouped exchanges.
/// It proves the SDK entry macro emits the async group and that the host and
/// SDK complete begin/resume cycles, including concurrent HTTP operations.

#include <coroutine>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace {

namespace sdk = yaddnsc::sdk;

class AsyncSdkDriver final : public sdk::AsyncDriver {
public:
    [[nodiscard]] sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext& context) override {
        std::string_view params = context.request().driver_param_json;
        if (const auto separator = params.find('|'); separator == std::string_view::npos) {
            sdk::HttpRequest request;
            request.url = params;
            auto response = co_await context.exchange(std::move(request));
            if (!response) {
                co_return failure(response.error());
            }
            co_return {};
        }

        const bool any = params.starts_with("any:") || params.starts_with("any-success:");
        const auto policy = params.starts_with("any-success:") ? sdk::AsyncWhenAnyPolicy::FIRST_SUCCESS
                                                               : sdk::AsyncWhenAnyPolicy::FIRST_COMPLETED;
        const auto prefix_end = params.find(':');
        params.remove_prefix(prefix_end + 1);
        const auto separator = params.find('|');
        std::vector<sdk::HttpRequest> requests;
        sdk::HttpRequest first;
        first.url = params.substr(0, separator);
        requests.push_back(std::move(first));
        sdk::HttpRequest second;
        second.url = params.substr(separator + 1);
        requests.push_back(std::move(second));

        if (!any) {
            auto operation = context.when_all(std::move(requests));
            if (!operation) {
                co_return failure(operation.error());
            }
            auto outcome = co_await std::move(*operation);
            if (outcome.update_cancelled_observed) {
                co_return std::unexpected(sdk::Error{YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0});
            }
            for (const auto& response : outcome.results) {
                if (!response) {
                    co_return failure(response.error());
                }
            }
            co_return {};
        }

        auto operation = context.when_any(std::move(requests), policy);
        if (!operation) {
            co_return failure(operation.error());
        }
        auto outcome = co_await std::move(*operation);
        if (outcome.update_cancelled_observed) {
            co_return std::unexpected(sdk::Error{YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0});
        }
        if (!outcome.winner_index) {
            co_return std::unexpected(sdk::Error{YADDNSC_STATUS_NETWORK_ERROR, "no request succeeded", 0});
        }
        const auto& winner = outcome.results[*outcome.winner_index];
        if (!winner) {
            co_return failure(winner.error());
        }
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view driver_param_json) const override {
        if (driver_param_json.empty()) {
            return std::unexpected(sdk::Error{YADDNSC_STATUS_INVALID_CONFIG, "empty driver_params", 0});
        }
        return {};
    }

private:
    static sdk::Result failure(const sdk::HttpError& error) {
        return std::unexpected(sdk::Error{error.status, error.message, error.retry_after_seconds});
    }
};

}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(AsyncSdkDriver, "async_sdk_driver", "Async SDK contract fixture", "yaddnsc", "1.0.0",
                            YADDNSC_DRIVER_CAPABILITY_A)
