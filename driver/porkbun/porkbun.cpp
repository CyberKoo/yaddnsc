//
// Created by Kotarou on 2026/7/13.
//

#include "porkbun.h"

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

namespace {
    constexpr std::string_view API_URL = "https://api.porkbun.com/api/json/v3/dns/editByNameType/{DOMAIN}/{TYPE}/{SUBDOMAIN}";
    constexpr std::string_view DRIVER_NAME = "porkbun";
}

YADDNSC_DEFINE_DRIVER(PorkbunDriver, "porkbun", "Updates DNS records via the Porkbun API", "Kotarou",
                      "1.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result PorkbunDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<PorkbunParams>(driver_param_json);
    return {};
}

Result PorkbunDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<PorkbunParams>(params.driver_param_json);

    HttpRequest request{};

    // Porkbun's editByNameType uses subdomain (not FQDN). Empty subdomain for root domain.
    const auto subdomain = (params.subdomain == "@" || params.subdomain.empty()) ? "" : params.subdomain;

    request.url = fmt::format(API_URL,
                              fmt::arg("DOMAIN", params.domain),
                              fmt::arg("TYPE", params.record_type),
                              fmt::arg("SUBDOMAIN", subdomain));
    // Use header auth (preferred per docs) and body auth as fallback
    request.headers.push_back({"X-API-Key", cfg.api_key});
    request.headers.push_back({"X-Secret-API-Key", cfg.secret_api_key});
    request.body = generate_body(cfg, params);
    request.content_type = "application/json";
    request.method = Method::Post;

    return run_update(context, DRIVER_NAME, request,
                      [](const HttpResponse &response, const Services &services) {
                          return check_response(response, services);
                      });
}

bool PorkbunDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    auto result = glz::read_json<PorkbunResponse>(response.body);
    if (!result) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Porkbun API response");
        return false;
    }

    auto &resp = result.value();
    if (resp.status == "SUCCESS") {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    if (resp.message.has_value()) {
        YADDNSC_SDK_LOG_ERROR(services, "Porkbun API error ({}): {}", resp.code.value_or("unknown"),
                              resp.message.value());
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Porkbun API request failed with status: {}", resp.status);
    }

    return false;
}

std::string PorkbunDriver::generate_body(const PorkbunParams &cfg, const UpdateRequest &request) {
    auto body = PorkbunRequestBody{
        .apikey = cfg.api_key,
        .secretapikey = cfg.secret_api_key,
        .content = std::string(request.ip_address),
        .ttl = cfg.ttl
    };
    return glz::write_json(body).value_or("{}");
}
