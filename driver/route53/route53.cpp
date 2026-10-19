#include "route53.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>

#include "config.hpp"
#include "sigv4.h"
#include "xml_payload.h"

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

// ── Helpers ──────────────────────────────────────────────────────────────

/// Ensure the FQDN has a trailing dot, as required by Route 53.
[[nodiscard]] std::string ensure_trailing_dot(std::string_view fqdn) {
    if (fqdn.empty())
        return ".";
    if (fqdn.back() == '.')
        return std::string(fqdn);
    return fmt::format("{}.", fqdn);
}

constexpr std::string_view DRIVER_NAME = "route53";

/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Route 53 API request";

}  // anonymous namespace

YADDNSC_DEFINE_ASYNC_DRIVER(Route53Driver, "route53", "Updates DNS records via the AWS Route 53 API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

// =============================================================================
//  Route53Driver::update
// =============================================================================

Route53Params Route53Driver::parse_params(std::string_view json) {
    auto cfg = parse_config<Route53Params>(json);
    require_non_empty("access_key_id", cfg.access_key_id);
    require_non_empty("secret_access_key", cfg.secret_access_key);
    require_non_empty("hosted_zone_id", cfg.hosted_zone_id);
    require_non_empty("region", cfg.region);
    require_non_empty("record_name", cfg.record_name);
    optional_non_negative("ttl", cfg.ttl);
    return cfg;
}

Result Route53Driver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> Route53Driver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    // Route 53 requires the FQDN with a trailing dot.
    auto fqdn = ensure_trailing_dot(params.fqdn);

    // Build the XML request body.
    auto ttl = cfg.ttl.value_or(300);
    auto body = route53::build_change_body(fqdn, params.record_type, params.ip_address, ttl);

    // URL path (used for both the request URL and the SigV4 canonical URI).
    constexpr std::string_view URI_PATH_PREFIX = "/2013-04-01/hostedzone/";
    auto url_path = fmt::format("{}{}/rrset", URI_PATH_PREFIX, cfg.hosted_zone_id);

    // Sign the exact payload that goes on the wire.
    auto signature = route53::sign_request(cfg.access_key_id, cfg.secret_access_key, cfg.region, url_path, body);

    // -----------------------------------------------------------------------
    //  Assemble the request
    // -----------------------------------------------------------------------
    HttpRequest request{};
    request.url = fmt::format("https://{}{}", route53::R53_HOST, url_path);
    request.method = Method::POST;
    request.content_type = "application/xml";
    request.body = std::move(body);
    request.headers.push_back({"Host", std::string(route53::R53_HOST)});
    request.headers.push_back({"X-Amz-Date", std::move(signature.amz_date)});
    request.headers.push_back({"X-Amz-Content-SHA256", std::move(signature.content_sha256)});
    request.headers.push_back({"Authorization", std::move(signature.authorization)});

    return run_update(context, DRIVER_NAME, request, check_response);
}

// =============================================================================
//  Route53Driver::check_response
// =============================================================================

Result Route53Driver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    if (response.status_code == 200) {
        // Route 53 returns HTTP 200 with <ChangeResourceRecordSetsResponse> on success.
        const auto change = route53::parse_change_response(response.body);
        if (!change.parsed) {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse Route 53 response XML");
            return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "Failed to parse Route 53 response XML"));
        }
        if (!change.namespace_registered) {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to register Route 53 XML namespace");
            return std::unexpected(
                yaddnsc::sdk::upstream_rejection(response, "Failed to register Route 53 XML namespace"));
        }
        if (!change.status) {
            YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Route 53 response missing <ChangeInfo><Status> element");
            return std::unexpected(
                yaddnsc::sdk::upstream_rejection(response, "Route 53 response missing <ChangeInfo><Status> element"));
        }

        const bool success = (*change.status == "PENDING" || *change.status == "INSYNC");
        if (success) {
            YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully (status: {})", *change.status);
            return {};
        }
        const auto text = fmt::format("Route 53 returned unexpected status: {}", *change.status);
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
    }

    // ── Error response: parse <ErrorResponse> XML ────────────────────────────
    if (!response.body.empty()) {
        const auto errors = route53::parse_error_response(response.body);
        if (!errors.empty()) {
            std::string detail;
            for (const auto& error : errors) {
                const auto text = fmt::format("Route 53 API error code {}: {}", error.code.value_or("no code"),
                                              error.message.value_or("unknown"));
                YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
                if (!detail.empty()) {
                    detail += "; ";
                }
                detail += text;
            }
            return std::unexpected(yaddnsc::sdk::upstream_rejection(response, detail));
        }
    }

    return std::unexpected(yaddnsc::sdk::status_rejection(services, response, REJECTION_SUBJECT));
}
