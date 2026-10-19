#include "digital_ocean.h"

#include <expected>
#include <optional>
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

namespace {
constexpr std::string_view API_URL = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "digital_ocean";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "DigitalOcean API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(DigitalOceanDriver, "digital_ocean", "Updates DNS records via the DigitalOcean API", "Kotarou",
                      "2.1.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

DigitalOceanParams DigitalOceanDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<DigitalOceanParams>(json);
    require_non_empty("record_id", cfg.record_id);
    require_non_empty("token", cfg.token);
    return cfg;
}

Result DigitalOceanDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> DigitalOceanDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = glz::write_json(DigitalOceanRequestBody{.data = std::string(params.ip_address)}).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::PUT;

    return run_update(context, DRIVER_NAME, request, check_response);
}

Result DigitalOceanDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // Success response: { "domain_record": { ... } }. Tolerant parsing means
    // any JSON object parses, so the presence of domain_record is what
    // distinguishes success from other shapes.
    if (auto result = parse_response<DigitalOceanDomainResponse>(response.body); result && result->domain_record) {
        auto& record = *result->domain_record;
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully: {} {} -> {} (TTL: {})", record.type,
                                    record.name, record.data, record.ttl);
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    // Error response: { "id": "...", "message": "..." }
    if (auto result = parse_response<DigitalOceanErrorResponse>(response.body);
        result && (!result->id.empty() || !result->message.empty())) {
        const auto text = fmt::format("DigitalOcean API error {}: {}", result->id, result->message);
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
    }

    YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse DigitalOcean API response");
    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "Failed to parse DigitalOcean API response"));
}
