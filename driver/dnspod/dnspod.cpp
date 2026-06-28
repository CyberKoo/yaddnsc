//
// Created by Kotarou on 2022/4/11.
//

#include "dnspod.h"

#include <unordered_map>

#include "fmt.hpp"
#include "config.hpp"
#include "response.h"
#include "driver/driver_factory.h"
#include "interfaces/core_logger.h"
#include "interfaces/http_client.h"

namespace {
    constexpr std::string_view API_URL_CN = "https://dnsapi.cn/Record.Ddns";

    constexpr std::string_view API_URL_GLOBAL = "https://api.dnspod.com/Record.Ddns";

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
}

DEFINE_DRIVER_FACTORY(DNSPodDriver)

driver_request DNSPodDriver::generate_request(const driver_config_type &config, const UpdateContext &ctx) const {
    auto cfg = parse_config<DNSPodParams>(config);

    // record_line: optional, with dynamic default based on global flag
    auto record_line = cfg.record_line.value_or(cfg.global ? "default" : "默认");

    driver_request request{};
    request.url = cfg.global ? API_URL_GLOBAL : API_URL_CN;
    request.body = HttpClient::params_to_query_string(driver_param_type{
        {"login_token", cfg.login_token},
        {"domain_id", cfg.domain_id},
        {"record_id", cfg.record_id},
        {"sub_domain", ctx.subdomain},
        {"record_type", ctx.rd_type},
        {"value", ctx.ip_addr},
        {"record_line", record_line},
        {"record_line_id", cfg.record_line_id},
        {"format", "json"}
    });
    request.content_type = "application/x-www-form-urlencoded";
    request.request_method = driver_http_method_type::POST;

    return request;
}

bool DNSPodDriver::check_response(std::string_view response) const {
    CORE_LOG_TRACE("Got {} from server.", response);

    auto result = glz::read_json<DnsPodResponse>(response);
    if (!result) {
        CORE_LOG_ERROR("Failed to parse DNSPod API response");
        return false;
    }

    auto resp = result.value();
    if (!resp.status.has_value()) {
        CORE_LOG_ERROR("Server returned an unknown error, raw response: {}", response);
        return false;
    }

    auto &status = resp.status.value();
    if (status.code == "1") {
        if (resp.record.has_value()) {
            auto &record = resp.record.value();
            CORE_LOG_DEBUG("Record updated successfully, id: {}, name: {}, value: {}", record.id, record.name,
                           record.value);
        }
        return true;
    }

    auto description = describe_error_code(status.code);
    CORE_LOG_ERROR("DNSPod API error: {} (code: {}, description: {})", status.message, status.code, description);

    return false;
}

DriverDetail DNSPodDriver::get_detail() const {
    return {
        .name = "dnspod",
        .description = "Updates DNS records via the DNSPod API",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}

std::string_view DNSPodDriver::describe_error_code(std::string_view code) {
    const auto it = ERROR_CODES.find(code);
    return it != ERROR_CODES.end() ? it->second : "Unknown error code";
}
