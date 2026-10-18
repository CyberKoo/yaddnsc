#include "cloudflare.h"

#include <optional>
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
using yaddnsc::sdk::UpdateRequest;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "cloudflare";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(CloudflareDriver, "cloudflare", "Updates DNS records via the Cloudflare API", "Kotarou", "2.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

CloudflareParams CloudflareDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<CloudflareParams>(json);
    if (cfg.zone_id.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'zone_id' must not be empty");
    }
    if (cfg.record_id.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'record_id' must not be empty");
    }
    if (cfg.token.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'token' must not be empty");
    }
    if (cfg.ttl && *cfg.ttl < 0) {
        throw yaddnsc::sdk::ConfigParseError("Parameter 'ttl' must not be negative");
    }
    return cfg;
}

Result CloudflareDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Task<Result> CloudflareDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("ZONE_ID", cfg.zone_id), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = generate_body(cfg, params);
    request.content_type = "application/json";
    request.method = Method::PUT;

    return run_update(context, DRIVER_NAME, request, check_response);
}

bool CloudflareDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    YADDNSC_ASYNC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    auto resp = parse_response<CloudflareResponse>(response.body);
    if (resp && resp->success) {
        if (resp->result.has_value()) {
            auto& record = resp->result.value();
            YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully: {} {} -> {} (TTL: {}, proxied: {})",
                                  record.type, record.name, record.content, record.ttl, record.proxied ? "yes" : "no");
        }
        return true;
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_ASYNC_SDK_LOG_WARN(services, "Cloudflare API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    if (!resp) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse Cloudflare API response");
        return false;
    }
    for (const auto& error : resp->errors) {
        if (error.source.has_value()) {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Cloudflare API error {} at {}: {}", error.code, error.source->pointer,
                                  error.message);
        } else {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Cloudflare API error {}: {}", error.code, error.message);
        }
    }

    return false;
}

std::string CloudflareDriver::generate_body(const CloudflareParams& cfg, const UpdateRequest& request) {
    auto body = CloudflareRequestBody{.type = std::string(request.record_type),
                                      .name = std::string(request.subdomain),
                                      .content = std::string(request.ip_address),
                                      .ttl = cfg.ttl.value_or(30),
                                      .proxied = cfg.proxied.value_or(false)};
    return glz::write_json(body).value_or("{}");
}
