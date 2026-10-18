#include "vultr.h"

#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

#include "config.hpp"
#include "response.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::AsyncHttpResponse;
using yaddnsc::sdk::AsyncServices;
using yaddnsc::sdk::AsyncUpdateContext;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Task;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://api.vultr.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "vultr";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(VultrDriver, "vultr", "Updates DNS records via the Vultr API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

VultrParams VultrDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<VultrParams>(json);
    if (cfg.api_key.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'api_key' must not be empty");
    }
    if (cfg.record_id.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'record_id' must not be empty");
    }
    if (cfg.ttl && *cfg.ttl < 0) {
        throw yaddnsc::sdk::ConfigParseError("Parameter 'ttl' must not be negative");
    }
    return cfg;
}

Result VultrDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Task<Result> VultrDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.api_key)});
    const auto body =
        VultrRequestBody{.name = std::string(params.subdomain), .data = std::string(params.ip_address), .ttl = cfg.ttl};
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::PATCH;

    return run_update(context, DRIVER_NAME, request, check_response);
}

bool VultrDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    YADDNSC_ASYNC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_ASYNC_SDK_LOG_WARN(services, "Vultr API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    // Vultr returns 204 No Content with an empty body on success.
    if (response.status_code == 204) {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with an error message.
    if (!response.body.empty()) {
        if (auto result = parse_response<VultrErrorResponse>(response.body); result && !result->error.empty()) {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Vultr API error with status {}: {}", result->status, result->error);
        } else {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Vultr API request failed with HTTP status {}: {}", response.status_code,
                                  truncate_for_log(response.body));
        }
    } else {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Vultr API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
