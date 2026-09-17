//
// Created by Kotarou on 2026/7/13.
//

#include "vultr.h"

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
    constexpr std::string_view API_URL = "https://api.vultr.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
    constexpr std::string_view DRIVER_NAME = "vultr";
}

YADDNSC_DEFINE_DRIVER(VultrDriver, "vultr", "Updates DNS records via the Vultr API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result VultrDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<VultrParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.api_key)});
    const auto body = VultrRequestBody{
        .name = std::string(params.subdomain),
        .data = std::string(params.ip_address),
        .ttl = cfg.ttl
    };
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::Patch;

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

bool VultrDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // Vultr returns 204 No Content with an empty body on success.
    if (response.status_code == 204) {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with error details.
    if (!response.body.empty()) {
        if (auto result = glz::read_json<VultrErrorResponse>(response.body)) {
            for (const auto &err: result.value().errors) {
                YADDNSC_SDK_LOG_ERROR(services, "Vultr API error: {}", err.detail);
            }
        } else {
            YADDNSC_SDK_LOG_ERROR(services, "Vultr API error (HTTP {}): {}", response.status_code, response.body);
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Vultr API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
