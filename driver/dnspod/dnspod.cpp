//
// Created by Kotarou on 2022/4/11.
//

#include "dnspod.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/form_encode.hpp>
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
constexpr std::string_view API_URL_CN = "https://dnsapi.cn/Record.Ddns";

constexpr std::string_view API_URL_GLOBAL = "https://api.dnspod.com/Record.Ddns";

constexpr std::string_view DRIVER_NAME = "dnspod";

std::unordered_map<std::string_view, std::string_view> ERROR_CODES = {
    {"-15", "Domain got prohibited"},
    {"-8", "You need a upgrade for the domain you are acting for"},
    {"-7", "A domain of a company account need a upgrade first"},
    {"-4", "Not in this agent"},
    {"-3", "Invalid agent"},
    {"-2", "API used too frequently"},
    {"-1", "Login fails"},
    {"1", "Action completed successfully"},
    {"2", "POST method only"},
    {"3", "Unknown errors"},
    {"6", "Invalid user_id / Invalid domain id"},
    {"7", "You don't have the permission / User is not under this agent"},
    {"8", "Invalid record id"},
    {"21", "Domain got locked"},
    {"22", "Invalid sub domain"},
    {"23", "The number of the record level is up to limit"},
    {"24", "Invalid sub domain for general analysis"},
    {"25", "The number of poll is up to limit"},
    {"26", "Invalid record line"},
    {"85", "Account logged-on in another place and your request got rejected"},
    {"-99", "This API is not ready to be used"},
};
}  // namespace

YADDNSC_DEFINE_DRIVER(DNSPodDriver,
                      "dnspod",
                      "Updates DNS records via the DNSPod API",
                      "Kotarou",
                      "2.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result DNSPodDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<DNSPodParams>(driver_param_json);
    return {};
}

Result DNSPodDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_config<DNSPodParams>(params.driver_param_json);

    auto request = generate_request(cfg, params);

    return run_update(context, DRIVER_NAME, request, [](const HttpResponse& response, const Services& services) {
        return check_response(response, services);
    });
}

HttpRequest DNSPodDriver::generate_request(const DNSPodParams& cfg, const UpdateRequest& params) {
    // record_line: optional, with dynamic default based on global flag
    auto record_line = cfg.record_line.value_or(cfg.global ? "default" : "默认");

    HttpRequest request{};
    request.url = std::string(cfg.global ? API_URL_GLOBAL : API_URL_CN);
    request.body = yaddnsc::sdk::encode_form({{"login_token", cfg.login_token},
                                              {"domain_id", cfg.domain_id},
                                              {"record_id", cfg.record_id},
                                              {"sub_domain", std::string(params.subdomain)},
                                              {"record_type", std::string(params.record_type)},
                                              {"value", std::string(params.ip_address)},
                                              {"record_line", record_line},
                                              {"record_line_id", cfg.record_line_id},
                                              {"format", "json"}});
    request.content_type = "application/x-www-form-urlencoded";
    request.method = Method::POST;

    return request;
}

bool DNSPodDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    auto result = glz::read_json<DnsPodResponse>(response.body);
    if (!result) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse DNSPod API response");
        return false;
    }

    auto resp = result.value();
    if (!resp.status.has_value()) {
        YADDNSC_SDK_LOG_ERROR(services, "Server returned an unknown error, raw response: {}", response.body);
        return false;
    }

    auto& status = resp.status.value();
    if (status.code == "1") {
        if (resp.record.has_value()) {
            auto& record = resp.record.value();
            YADDNSC_SDK_LOG_DEBUG(services, "Record updated successfully, id: {}, name: {}, value: {}", record.id,
                                  record.name, record.value);
        }
        return true;
    }

    auto description = describe_error_code(status.code);
    YADDNSC_SDK_LOG_ERROR(services, "DNSPod API error: {} (code: {}, description: {})", status.message, status.code,
                          description);

    return false;
}

std::string_view DNSPodDriver::describe_error_code(std::string_view code) {
    const auto it = ERROR_CODES.find(code);
    return it != ERROR_CODES.end() ? it->second : "Unknown error code";
}
