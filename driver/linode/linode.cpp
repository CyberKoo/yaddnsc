#include "linode.h"

#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>

#include "config.hpp"
#include "response.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;
using yaddnsc::sdk::UpdateRequest;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://api.linode.com/v4/domains/{DOMAIN_ID}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "linode";
}  // namespace

YADDNSC_DEFINE_DRIVER(LinodeDriver, "linode", "Updates DNS records via the Linode API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)

LinodeParams LinodeDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<LinodeParams>(json);
    if (cfg.token.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'token' must not be empty");
    }
    if (cfg.domain_id.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'domain_id' must not be empty");
    }
    if (cfg.record_id.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'record_id' must not be empty");
    }
    if (cfg.ttl_sec && *cfg.ttl_sec < 0) {
        throw yaddnsc::sdk::ConfigParseError("Parameter 'ttl_sec' must not be negative");
    }
    return cfg;
}

Result LinodeDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Result LinodeDriver::update(UpdateContext& context) {
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

bool LinodeDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_SDK_LOG_WARN(services, "Linode API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    // Linode returns 200 OK with the updated record object on success.
    if (response.status_code == 200) {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with error details.
    if (!response.body.empty()) {
        if (auto result = parse_response<LinodeErrorResponse>(response.body); result && !result->errors.empty()) {
            for (const auto& err : result->errors) {
                if (err.field.empty()) {
                    YADDNSC_SDK_LOG_ERROR(services, "Linode API error: {}", err.reason);
                } else {
                    YADDNSC_SDK_LOG_ERROR(services, "Linode API error in field {}: {}", err.field, err.reason);
                }
            }
        } else {
            YADDNSC_SDK_LOG_ERROR(services, "Linode API request failed with HTTP status {}: {}", response.status_code,
                                  truncate_for_log(response.body));
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Linode API request failed with HTTP status {}", response.status_code);
    }

    return false;
}

std::string LinodeDriver::generate_body(const UpdateRequest& request, std::optional<int> ttl_sec) {
    auto body = LinodeRequestBody{
        .name = std::string(request.subdomain), .target = std::string(request.ip_address), .ttl_sec = ttl_sec};
    return glz::write_json(body).value_or("{}");
}
