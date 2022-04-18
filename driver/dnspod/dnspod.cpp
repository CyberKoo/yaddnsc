//
// Created by Kotarou on 2022/4/11.
//
#include <nlohmann/json.hpp>

#include "dnspod.h"
#include "string_util.h"

constexpr char API_URL_CN[] = "https://dnsapi.cn/Domain.List";

constexpr char API_URL_GLOBAL[] = "https://api.dnspod.com/Record.Ddns";

DNSPodDriver::DNSPodDriver() {
    _required_param.emplace_back("domain_id");
    _required_param.emplace_back("record_id");
    _required_param.emplace_back("subdomain");
    _required_param.emplace_back("login_token");
    _required_param.emplace_back("ip_addr");
}

driver_request_t DNSPodDriver::generate_request(const driver_config_t &config) {
    check_required_params(config);
    auto is_global = StringUtil::str_to_bool(get_optional(config, "global").value_or("false"));

    driver_request_t request{};
    request.url = is_global ? API_URL_GLOBAL : API_URL_CN;
    request.body = driver_param_t{
            {"login_token", config.at("login_token")},
            {"domain_id",   config.at("domain_id")},
            {"record_id",   config.at("record_id")},
            {"sub_domain",  config.at("subdomain")},
            {"record_line", get_optional(config, "redord_line").value_or("default")},
            {"value",       config.at("ip_addr")},
            {"format",      "json"}
    };
    request.content_type = "application/json";
    request.request_method = driver_http_method_t::POST;

    return request;
}

bool DNSPodDriver::check_response(std::string_view response) {
    SPDLOG_TRACE("Got {} from server.", response);
    auto json = nlohmann::json::parse(response);
    if (json.contains("status")) {
        auto &status = json["status"];
        if (status["code"].get<std::string>() == "1") {
            return true;
        } else {
            SPDLOG_ERROR("Error from server: {}, code: {}", status["message"].get<std::string>(),
                         status["code"].get<std::string>());
        }
    } else {
        SPDLOG_ERROR("Server return an unknown error, raw response: {}", response);
    }

    return false;
}

driver_detail_t DNSPodDriver::get_detail() {
    return {
            .name = "dnspod",
            .description="DNSPod DDNS driver",
            .author="Kotarou",
            .version = "1.0.0"
    };
}
