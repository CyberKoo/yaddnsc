#include "porkbun.h"

#include <optional>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

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
constexpr std::string_view API_URL =
    "https://api.porkbun.com/api/json/v3/dns/editByNameType/{DOMAIN}/{TYPE}/{SUBDOMAIN}";
constexpr std::string_view DRIVER_NAME = "porkbun";
}  // namespace

YADDNSC_DEFINE_DRIVER(PorkbunDriver, "porkbun", "Updates DNS records via the Porkbun API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)

PorkbunParams PorkbunDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<PorkbunParams>(json);
    if (cfg.api_key.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'api_key' must not be empty");
    }
    if (cfg.secret_api_key.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'secret_api_key' must not be empty");
    }
    if (cfg.ttl && *cfg.ttl < 0) {
        throw yaddnsc::sdk::ConfigParseError("Parameter 'ttl' must not be negative");
    }
    return cfg;
}

Result PorkbunDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Result PorkbunDriver::update(UpdateContext& context) {
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

bool PorkbunDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    auto resp = parse_response<PorkbunResponse>(response.body);
    if (resp && resp->status == "SUCCESS") {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_SDK_LOG_WARN(services, "Porkbun API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    if (!resp) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Porkbun API response");
        return false;
    }

    if (resp->message.has_value()) {
        YADDNSC_SDK_LOG_ERROR(services, "Porkbun API error {}: {}", resp->code.value_or("unknown"),
                              resp->message.value());
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Porkbun API request failed with status: {}", resp->status);
    }

    return false;
}

std::string PorkbunDriver::generate_body(const PorkbunParams& cfg, const UpdateRequest& request) {
    auto body = PorkbunRequestBody{.apikey = cfg.api_key,
                                   .secretapikey = cfg.secret_api_key,
                                   .content = std::string(request.ip_address),
                                   .ttl = cfg.ttl};
    return glz::write_json(body).value_or("{}");
}
