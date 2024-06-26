//
// Created by Kotarou on 2022/4/11.
//
#include <nlohmann/json.hpp>

#include "dnspod.h"
#include "string_util.h"

constexpr char API_URL_CN[] = "https://dnsapi.cn/Record.Ddns";

constexpr char API_URL_GLOBAL[] = "https://api.dnspod.com/Record.Ddns";

DNSPodDriver::DNSPodDriver() {
    required_param_.emplace_back("domain_id");
    required_param_.emplace_back("record_id");
    required_param_.emplace_back("subdomain");
    required_param_.emplace_back("login_token");
    required_param_.emplace_back("ip_addr");
}

driver_request DNSPodDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);
    auto is_global = StringUtil::str_to_bool(get_optional(config, "global").value_or("false"));

    driver_request request{};
    request.url = is_global ? API_URL_GLOBAL : API_URL_CN;
    request.body = driver_param_type{
        {"login_token", config.at("login_token")},
        {"domain_id", config.at("domain_id")},
        {"record_id", config.at("record_id")},
        {"sub_domain", config.at("subdomain")},
        {"record_type", config.at("rd_type")},
        {"record_line", get_optional(config, "record_line").value_or("默认")},
        {"record_line_id", get_optional(config, "record_line_id").value_or("0")},
        {"value", config.at("ip_addr")},
        {"format", "json"}
    };
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::POST;

    return request;
}

bool DNSPodDriver::check_response(std::string_view response) const {
    SPDLOG_TRACE("Got {} from server.", response);
    if (auto json = nlohmann::json::parse(response); json.contains("status")) {
        auto &status = json["status"];
        if (status["code"].get<std::string>() == "1") {
            return true;
        }

        SPDLOG_ERROR("Error from server: {}, code: {}", status["message"].get<std::string>(),
                     status["code"].get<std::string>());
    } else {
        SPDLOG_ERROR("Server return an unknown error, raw response: {}", response);
    }

    return false;
}

driver_detail DNSPodDriver::get_detail() const {
    return {
        .name = "dnspod",
        .description = "DNSPod DDNS driver",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
