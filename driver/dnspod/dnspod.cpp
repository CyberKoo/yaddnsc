//
// Created by Kotarou on 2022/4/11.
//

#include "dnspod.h"

#include "response.h"
#include "string_util.h"

DEFINE_DRIVER_CREATE(DNSPodDriver)

constexpr char API_URL_CN[] = "https://dnsapi.cn/Record.Ddns";

constexpr char API_URL_GLOBAL[] = "https://api.dnspod.com/Record.Ddns";

driver_request DNSPodDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);
    auto is_global = StringUtil::str_to_bool(get_optional(config, "global").value_or("false"));
    auto default_record_line = is_global ? "default" : "默认";

    driver_request request{};
    request.url = is_global ? API_URL_GLOBAL : API_URL_CN;
    request.body = driver_param_type{
        {"login_token", config.at("login_token")},
        {"domain_id", config.at("domain_id")},
        {"record_id", config.at("record_id")},
        {"sub_domain", config.at("subdomain")},
        {"record_type", config.at("rd_type")},
        {"record_line", get_optional(config, "record_line").value_or(default_record_line)},
        {"record_line_id", get_optional(config, "record_line_id").value_or("0")},
        {"value", config.at("ip_addr")},
        {"format", "json"}
    };
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::POST;

    return request;
}

std::string_view DNSPodDriver::describe_error_code(std::string_view code) {
    static constexpr std::pair<std::string_view, std::string_view> known_codes[] = {
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

    for (auto &[c, desc]: known_codes) {
        if (c == code) {
            return desc;
        }
    }

    return "Unknown error code";
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
            CORE_LOG_INFO("Record updated successfully, id: {}, name: {}, value: {}", record.id, record.name,
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
        .description = "DNSPod DDNS driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
