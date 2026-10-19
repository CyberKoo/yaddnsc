#include "linode.h"

#include <expected>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>

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
using yaddnsc::sdk::UpdateRequest;

namespace {
constexpr std::string_view API_URL = "https://api.linode.com/v4/domains/{DOMAIN_ID}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "linode";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Linode API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(LinodeDriver, "linode", "Updates DNS records via the Linode API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

LinodeParams LinodeDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<LinodeParams>(json);
    require_non_empty("token", cfg.token);
    require_non_empty("domain_id", cfg.domain_id);
    require_non_empty("record_id", cfg.record_id);
    optional_non_negative("ttl_sec", cfg.ttl_sec);
    return cfg;
}

Result LinodeDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> LinodeDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN_ID", cfg.domain_id), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = generate_body(params, cfg.ttl_sec);
    request.content_type = "application/json";
    request.method = Method::PUT;

    return run_update(context, DRIVER_NAME, request, check_response);
}

Result LinodeDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    // Linode returns 200 OK with the updated record object on success.
    if (response.status_code == 200) {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return {};
    }

    // Error responses include a JSON body with error details.
    if (auto result = parse_response<LinodeErrorResponse>(response.body); result && !result->errors.empty()) {
        std::string detail;
        for (const auto& err : result->errors) {
            const auto text = err.field.empty()
                                  ? fmt::format("Linode API error: {}", err.reason)
                                  : fmt::format("Linode API error in field {}: {}", err.field, err.reason);
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
            if (!detail.empty()) {
                detail += "; ";
            }
            detail += text;
        }
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, detail));
    }

    return std::unexpected(yaddnsc::sdk::status_rejection(services, response, REJECTION_SUBJECT));
}

std::string LinodeDriver::generate_body(const UpdateRequest& request, std::optional<int> ttl_sec) {
    auto body = LinodeRequestBody{
        .name = std::string(request.subdomain), .target = std::string(request.ip_address), .ttl_sec = ttl_sec};
    return glz::write_json(body).value_or("{}");
}
