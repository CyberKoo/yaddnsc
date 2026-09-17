//
// Created by Kotarou on 2022/4/5.
//

#include "digital_ocean.h"

#include "response.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;

namespace {
    constexpr std::string_view API_URL = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
    constexpr std::string_view DRIVER_NAME = "digital_ocean";
}

YADDNSC_DEFINE_DRIVER(DigitalOceanDriver, "digital_ocean", "Updates DNS records via the DigitalOcean API", "Kotarou",
                      "2.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result DigitalOceanDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<DigitalOceanParams>(driver_param_json);
    return {};
}

Result DigitalOceanDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<DigitalOceanParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = glz::write_json(DigitalOceanBody{.data = std::string(params.ip_address)}).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::Put;

    return run_update(context, DRIVER_NAME, request,
                      [](const HttpResponse &response, const Services &services) {
                          return check_response(response, services);
                      });
}

bool DigitalOceanDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // Try success response: { "domain_record": { ... } }
    if (auto result = glz::read_json<DigitalOceanDomainResponse>(response.body)) {
        auto &record = result.value().domain_record;
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully: {} {} -> {} (TTL: {})", record.type,
                              record.name, record.data, record.ttl);
        return true;
    }

    // Try error response: { "id": "...", "message": "..." }
    if (auto result = glz::read_json<DigitalOceanErrorResponse>(response.body)) {
        auto &err = result.value();
        YADDNSC_SDK_LOG_ERROR(services, "DigitalOcean API error ({}): {}", err.id, err.message);
        return false;
    }

    YADDNSC_SDK_LOG_ERROR(services, "Failed to parse DigitalOcean API response");
    return false;
}
