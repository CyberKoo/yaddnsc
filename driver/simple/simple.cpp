#include "simple.h"

#include <optional>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::optional member complete; clangd sees no spelled use
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use

#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>
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
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view DRIVER_NAME = "simple";
}  // namespace

YADDNSC_DEFINE_DRIVER(SimpleDriver, "simple", "Generic HTTP driver with URL template substitution", "Kotarou", "2.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)

Result SimpleDriver::update(UpdateContext& context) {
    const auto& params = context.request();

    auto request = generate_request(params);

    return run_update(context, DRIVER_NAME, request, check_response);
}

Result SimpleDriver::validate(std::string_view driver_param_json) const {
    // Same check as the update path: a missing or non-string "url" throws
    // ConfigParseError, which the ABI entry maps to YADDNSC_STATUS_INVALID_CONFIG.
    std::ignore = parse_driver_param(driver_param_json);
    return {};
}

glz::generic SimpleDriver::parse_driver_param(std::string_view driver_param_json) {
    auto full = parse_config<glz::generic>(driver_param_json);
    if (!full.is_object() || !full.contains("url") || !full["url"].is_string() || full["url"].get_string().empty()) {
        throw yaddnsc::sdk::ConfigParseError(
            "Driver configuration parse error: Missing required parameter \"url\" in driver config");
    }
    return full;
}

HttpRequest SimpleDriver::generate_request(const UpdateRequest& params) {
    auto full = parse_driver_param(params.driver_param_json);
    auto& obj = full.get_object();
    std::unordered_map<std::string, std::string> values;
    for (auto& [key, val] : obj) {
        if (key != "url" && val.is_string())
            values[key] = val.get_string();
    }
    // Config parameters retain their existing precedence over context fields.
    values.try_emplace("ip_addr", params.ip_address);
    values.try_emplace("rd_type", params.record_type);
    values.try_emplace("domain", params.domain);
    values.try_emplace("subdomain", params.subdomain);
    values.try_emplace("fqdn", params.fqdn);
    auto url = fmt::format_named(obj["url"].get_string(), values);

    HttpRequest request{};
    request.url = std::move(url);
    request.method = Method::GET;
    return request;
}

bool SimpleDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_DEBUG(services, "Status: {}, Response: {}", response.status_code,
                          truncate_for_log(string_util::trim(response.body)));

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_SDK_LOG_WARN(services, "HTTP request failed with status code {}: {}", response.status_code,
                             truncate_for_log(string_util::trim(response.body)));
        return false;
    }

    if (response.status_code >= 300) {
        YADDNSC_SDK_LOG_ERROR(services, "HTTP request failed with status code {}", response.status_code);
        return false;
    }

    return !response.body.empty();
}
