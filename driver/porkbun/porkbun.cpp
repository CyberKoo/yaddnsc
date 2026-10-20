#include "porkbun.h"

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
using yaddnsc::sdk::UpdateRequest;

namespace {
constexpr std::string_view API_URL =
    "https://api.porkbun.com/api/json/v3/dns/editByNameType/{DOMAIN}/{TYPE}/{SUBDOMAIN}";
constexpr std::string_view DRIVER_NAME = "porkbun";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Porkbun API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(PorkbunDriver, "porkbun", "Updates DNS records via the Porkbun API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

PorkbunParams PorkbunDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<PorkbunParams>(json);
    require_non_empty("api_key", cfg.api_key);
    require_non_empty("secret_api_key", cfg.secret_api_key);
    optional_non_negative("ttl", cfg.ttl);
    return cfg;
}

Result PorkbunDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> PorkbunDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};

    // Porkbun's editByNameType uses subdomain (not FQDN). Empty subdomain for root domain.
    const auto subdomain = (params.subdomain == "@" || params.subdomain.empty()) ? "" : params.subdomain;

    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("TYPE", params.record_type),
                              fmt::arg("SUBDOMAIN", subdomain));
    // Use header auth (preferred per docs) and body auth as fallback
    request.headers.push_back({"X-API-Key", cfg.api_key});
    request.headers.push_back({"X-Secret-API-Key", cfg.secret_api_key});
    request.body = generate_body(cfg, params);
    request.content_type = "application/json";
    request.method = Method::POST;

    return run_update(context, DRIVER_NAME, request, check_response);
}

Result PorkbunDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    auto resp = parse_response<PorkbunResponse>(response.body);
    if (resp && resp->status == "SUCCESS") {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    if (!resp) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse Porkbun API response");
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "Failed to parse Porkbun API response"));
    }

    if (resp->message.has_value()) {
        const auto text =
            fmt::format("Porkbun API error {}: {}", resp->code.value_or("unknown"), resp->message.value());
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
    }

    const auto text = fmt::format("Porkbun API request failed with status: {}", resp->status);
    YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
}

std::string PorkbunDriver::generate_body(const PorkbunParams& cfg, const UpdateRequest& request) {
    auto body = PorkbunRequestBody{.apikey = cfg.api_key,
                                   .secretapikey = cfg.secret_api_key,
                                   .content = std::string(request.ip_address),
                                   .ttl = cfg.ttl};
    return glz::write_json(body).value_or("{}");
}
