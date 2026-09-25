//
// Created by Kotarou on 2026/7/13.
//

#include "vultr.h"

#include <optional>
#include <string>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

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

namespace {
constexpr std::string_view API_URL = "https://api.vultr.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "vultr";
}  // namespace

YADDNSC_DEFINE_DRIVER(VultrDriver,
                      "vultr",
                      "Updates DNS records via the Vultr API",
                      "Kotarou",
                      "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result VultrDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<VultrParams>(driver_param_json);
    return {};
}

Result VultrDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_config<VultrParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.api_key)});
    const auto body =
        VultrRequestBody{.name = std::string(params.subdomain), .data = std::string(params.ip_address), .ttl = cfg.ttl};
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = Method::PATCH;

    return run_update(context, DRIVER_NAME, request, [](const HttpResponse& response, const Services& services) {
        return check_response(response, services);
    });
}

bool VultrDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // Vultr returns 204 No Content with an empty body on success.
    if (response.status_code == 204) {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with an error message.
    if (!response.body.empty()) {
        if (auto result = parse_response<VultrErrorResponse>(response.body); result && !result->error.empty()) {
            YADDNSC_SDK_LOG_ERROR(services, "Vultr API error (status {}): {}", result->status, result->error);
        } else {
            YADDNSC_SDK_LOG_ERROR(services, "Vultr API error (HTTP {}): {}", response.status_code, response.body);
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Vultr API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
