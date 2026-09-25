//
// Created by Kotarou on 2026/7/13.
//

#include "linode.h"

#include <vector>

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
using yaddnsc::sdk::UpdateRequest;

namespace {
constexpr std::string_view API_URL = "https://api.linode.com/v4/domains/{DOMAIN_ID}/records/{RECORD_ID}";
constexpr std::string_view DRIVER_NAME = "linode";
}  // namespace

YADDNSC_DEFINE_DRIVER(LinodeDriver,
                      "linode",
                      "Updates DNS records via the Linode API",
                      "Kotarou",
                      "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result LinodeDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<LinodeParams>(driver_param_json);
    return {};
}

Result LinodeDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_config<LinodeParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN_ID", cfg.domain_id), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = generate_body(params, cfg.ttl_sec);
    request.content_type = "application/json";
    request.method = Method::PUT;

    return run_update(context, DRIVER_NAME, request, [](const HttpResponse& response, const Services& services) {
        return check_response(response, services);
    });
}

bool LinodeDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // Linode returns 200 OK with the updated record object on success.
    if (response.status_code == 200) {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with error details.
    if (!response.body.empty()) {
        if (auto result = parse_response<LinodeErrorResponse>(response.body); result && !result->errors.empty()) {
            for (const auto& err : result->errors) {
                YADDNSC_SDK_LOG_ERROR(services, "Linode API error{}: {}",
                                      err.field.empty() ? "" : fmt::format(" ({})", err.field), err.reason);
            }
        } else {
            YADDNSC_SDK_LOG_ERROR(services, "Linode API error (HTTP {}): {}", response.status_code, response.body);
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Linode API request failed with HTTP status {}", response.status_code);
    }

    return false;
}

std::string LinodeDriver::generate_body(const UpdateRequest& request, std::optional<int> ttl_sec) {
    auto body = LinodeRequestBody{
        .name = std::string(request.subdomain), .target = std::string(request.ip_address), .ttl_sec = ttl_sec};
    return glz::write_json(body).value_or("{}");
}
