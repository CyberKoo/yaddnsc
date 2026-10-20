#include "dnspod.h"

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>  // IWYU pragma: keep — ERROR_CODES' braced entries construct std::pair; clangd sees no spelled use
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/form_encode.hpp>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

#include "config.hpp"
#include "response.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::AsyncHttpResponse;
using yaddnsc::sdk::AsyncServices;
using yaddnsc::sdk::AsyncUpdateContext;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Task;
using yaddnsc::sdk::UpdateRequest;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL_CN = "https://dnsapi.cn/Record.Ddns";

constexpr std::string_view API_URL_GLOBAL = "https://api.dnspod.com/Record.Ddns";

constexpr std::string_view DRIVER_NAME = "dnspod";

/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "DNSPod API request";

const std::unordered_map<std::string_view, std::string_view> ERROR_CODES = {
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

YADDNSC_DEFINE_ASYNC_DRIVER(DnsPodDriver, "dnspod", "Updates DNS records via the DNSPod API", "Kotarou", "2.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

DnsPodParams DnsPodDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<DnsPodParams>(json);
    require_non_empty("domain_id", cfg.domain_id);
    require_non_empty("record_id", cfg.record_id);
    require_non_empty("login_token", cfg.login_token);
    require_non_empty("record_line", cfg.record_line);
    require_non_empty("record_line_id", cfg.record_line_id);
    return cfg;
}

Result DnsPodDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> DnsPodDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    auto request = generate_request(cfg, params);

    return run_update(context, DRIVER_NAME, request, check_response);
}

HttpRequest DnsPodDriver::generate_request(const DnsPodParams& cfg, const UpdateRequest& params) {
    // record_line: optional, with dynamic default based on global flag
    auto record_line = cfg.record_line.value_or(cfg.global.value_or(false) ? "default" : "默认");

    HttpRequest request{};
    request.url = std::string(cfg.global.value_or(false) ? API_URL_GLOBAL : API_URL_CN);
    request.body = yaddnsc::sdk::encode_form({{"login_token", cfg.login_token},
                                              {"domain_id", cfg.domain_id},
                                              {"record_id", cfg.record_id},
                                              {"sub_domain", std::string(params.subdomain)},
                                              {"record_type", std::string(params.record_type)},
                                              {"value", std::string(params.ip_address)},
                                              {"record_line", record_line},
                                              {"record_line_id", cfg.record_line_id.value_or("0")},
                                              {"format", "json"}});
    request.content_type = "application/x-www-form-urlencoded";
    request.method = Method::POST;

    return request;
}

Result DnsPodDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    auto resp = parse_response<DnsPodResponse>(response.body);
    if (resp && resp->status.has_value() && resp->status->code == "1") {
        if (resp->record.has_value()) {
            auto& record = resp->record.value();
            YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "Record updated successfully, id: {}, name: {}, value: {}", record.id,
                                        record.name, record.value);
        }
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    if (!resp) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse DNSPod API response");
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "Failed to parse DNSPod API response"));
    }

    if (!resp->status.has_value()) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Server returned an unknown error, raw response: {}",
                                    truncate_for_log(response.body));
        return std::unexpected(yaddnsc::sdk::upstream_rejection(response, "DNSPod API returned an unknown error"));
    }

    auto& status = resp->status.value();
    auto description = describe_error_code(status.code);
    const auto text =
        fmt::format("DNSPod API error code {} with message {}: {}", status.code, status.message, description);
    YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);

    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
}

std::string_view DnsPodDriver::describe_error_code(std::string_view code) {
    const auto it = ERROR_CODES.find(code);
    return it != ERROR_CODES.end() ? it->second : "Unknown error code";
}
