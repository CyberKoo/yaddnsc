//
// Created by Kotarou on 2022/4/5.
//

#include "simple.h"

#include <string>
#include <string_view>
#include <tuple>

#include <glaze/glaze.hpp>

#include <yaddnsc/sdk/string_util.hpp>

namespace fmt = yaddnsc::sdk::fmt;
namespace string_util = yaddnsc::sdk::string_util;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;
using yaddnsc::sdk::UpdateRequest;

namespace {
    constexpr std::string_view DRIVER_NAME = "simple";
} // namespace

YADDNSC_DEFINE_DRIVER(SimpleDriver, "simple", "Generic HTTP driver with URL template substitution", "Kotarou",
                      "2.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result SimpleDriver::update(UpdateContext &context) {
    const auto &params = context.request();

    auto request = generate_request(params);

    return run_update(context, DRIVER_NAME, request,
                      [this](const HttpResponse &response, const Services &services) {
                          return check_response(response, services);
                      });
}

Result SimpleDriver::validate(std::string_view driver_param_json) const {
    // Same check as the update path: a missing or non-string "url" throws
    // ConfigParseError, which the ABI entry maps to YADDNSC_STATUS_INVALID_CONFIG.
    std::ignore = parse_driver_param(driver_param_json);
    return {};
}

glz::generic SimpleDriver::parse_driver_param(std::string_view driver_param_json) {
    auto full = parse_config<glz::generic>(driver_param_json);
    if (!full.is_object() || !full.contains("url") || !full["url"].is_string()) {
        throw yaddnsc::sdk::ConfigParseError(
                "Driver configuration parse error: Missing required parameter \"url\" in driver config");
    }
    return full;
}

HttpRequest SimpleDriver::generate_request(const UpdateRequest &params) {
    auto full = parse_driver_param(params.driver_param_json);
    auto &obj = full.get_object();
    auto url = obj["url"].get_string();

    // Substitute all keys into the URL template: config params first, then context
    const auto substitute = [&](std::string_view key, std::string_view val) {
        const auto target = fmt::format("{{{}}}", key);
        string_util::replace_all(url, target, val);
    };

    for (auto &[key, val]: obj) {
        if (key != "url" && val.is_string()) {
            substitute(key, val.get_string());
        }
    }

    substitute("ip_addr", params.ip_address);
    substitute("rd_type", params.record_type);
    substitute("domain", params.domain);
    substitute("subdomain", params.subdomain);
    substitute("fqdn", params.fqdn);

    HttpRequest request{};
    request.url = std::move(url);
    request.method = Method::Get;
    return request;
}

bool SimpleDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_DEBUG(services, "Status: {}, Response: {}", response.status_code, string_util::trim(response.body));

    if (response.status_code >= 300) {
        YADDNSC_SDK_LOG_ERROR(services, "HTTP request failed with status code {}", response.status_code);
        return false;
    }

    return !response.body.empty();
}
