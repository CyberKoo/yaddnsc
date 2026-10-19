#include "cloudflare.h"

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
using yaddnsc::sdk::UpdateRequest;

namespace {
constexpr std::string_view API_URL = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "cloudflare";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Cloudflare API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(CloudflareDriver, "cloudflare", "Updates DNS records via the Cloudflare API", "Kotarou", "2.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

CloudflareParams CloudflareDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<CloudflareParams>(json);
    require_non_empty("zone_id", cfg.zone_id);
    require_non_empty("record_id", cfg.record_id);
    require_non_empty("token", cfg.token);
    optional_non_negative("ttl", cfg.ttl);
    return cfg;
}

Result CloudflareDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
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

Result CloudflareDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    auto resp = parse_response<CloudflareResponse>(response.body);
    if (resp && resp->success) {
        if (resp->result.has_value()) {
            auto& record = resp->result.value();
            YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully: {} {} -> {} (TTL: {}, proxied: {})",
                                        record.type, record.name, record.content, record.ttl,
                                        record.proxied ? "yes" : "no");
        }
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    if (!resp) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse Cloudflare API response");
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "Failed to parse Cloudflare API response"));
    }

    std::string detail;
    for (const auto& error : resp->errors) {
        const auto text =
            error.source.has_value()
                ? fmt::format("Cloudflare API error {} at {}: {}", error.code, error.source->pointer, error.message)
                : fmt::format("Cloudflare API error {}: {}", error.code, error.message);
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += text;
    }

    if (detail.empty()) {
        detail = yaddnsc::sdk::rejection_message(REJECTION_SUBJECT, response);
    }
    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, detail));
}

std::string CloudflareDriver::generate_body(const CloudflareParams& cfg, const UpdateRequest& request) {
    auto body = CloudflareRequestBody{.type = std::string(request.record_type),
                                      .name = std::string(request.subdomain),
                                      .content = std::string(request.ip_address),
                                      .ttl = cfg.ttl.value_or(30),
                                      .proxied = cfg.proxied.value_or(false)};
    return glz::write_json(body).value_or("{}");
}
