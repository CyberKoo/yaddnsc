#include "vultr.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

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

namespace {
constexpr std::string_view API_URL = "https://api.vultr.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "vultr";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Vultr API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(VultrDriver, "vultr", "Updates DNS records via the Vultr API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

VultrParams VultrDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<VultrParams>(json);
    require_non_empty("api_key", cfg.api_key);
    require_non_empty("record_id", cfg.record_id);
    optional_non_negative("ttl", cfg.ttl);
    return cfg;
}

Result VultrDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
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

Result VultrDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    // Vultr returns 204 No Content with an empty body on success.
    if (response.status_code == 204) {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return {};
    }

    // Error responses include a JSON body with an error message.
    if (auto result = parse_response<VultrErrorResponse>(response.body); result && !result->error.empty()) {
        const auto text = fmt::format("Vultr API error with status {}: {}", result->status, result->error);
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
    }

    return std::unexpected(yaddnsc::sdk::status_rejection(services, response, REJECTION_SUBJECT));
}
