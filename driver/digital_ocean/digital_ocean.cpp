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

Result DigitalOceanDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<DigitalOceanParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = glz::write_json(DigitalOceanBody{.data = std::string(params.ip_address)}).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::Put;

    YADDNSC_SDK_LOG_DEBUG(context, "Domain {} ({}) received DNS record update request from driver {}, {}",
                          params.fqdn, params.record_type, DRIVER_NAME, yaddnsc::sdk::format_request(request));

    auto response = context.exchange(request);
    if (!response) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update failed (HTTP error: {})", params.fqdn,
                             params.record_type, response.error().message);
        return std::unexpected(Error{response.error().status, response.error().message, 0});
    }

    if (!check_response(*response, context.services())) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type);
        return std::unexpected(Error{YADDNSC_STATUS_UPSTREAM_REJECTED,
                                     fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                 params.record_type),
                                     0});
    }

    return {};
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
