//
// Created by Kotarou on 2022/4/5.
//
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "digital_ocean.h"

constexpr char API_URL[] = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";

DigitalOceanDriver::DigitalOceanDriver() {
    required_param.emplace_back("domain");
    required_param.emplace_back("record_id");
    required_param.emplace_back("token");
    required_param.emplace_back("ip_addr");
}

request_t DigitalOceanDriver::generate_request(const driver_config_t &config) {
    check_required_params(config);

    request_t request{};
    // request.header.insert({"Content-Type", "application/json"});
    request.header.insert({"Authorization", fmt::format("Bearer {}", config.at("token"))});
    request.url = vformat(API_URL, {
            {"DOMAIN",    config.at("domain")},
            {"RECORD_ID", config.at("record_id")}
    });
    request.body = nlohmann::json({{"data", config.at("ip_addr")}}).dump();
    request.content_type = "application/json";
    request.request_method = request_method_t::PUT;
    
    return request;
}

bool DigitalOceanDriver::check_response(std::string_view response) {
    SPDLOG_TRACE("Got {} from server.", response);
    auto json = nlohmann::json::parse(response);
    if (json.contains("domain_record")) {
        SPDLOG_INFO("Successfully updated DNS record");
        return true;
    } else {
        if (json.contains("message")) {
            SPDLOG_ERROR("Error from server: {}", json["message"].get<std::string>());
        } else {
            SPDLOG_ERROR("Server return an unknown error, raw response: {}", response);
        }

        return false;
    }
}

driver_detail_t DigitalOceanDriver::get_detail() {
    return {
            .name = "digital_ocean",
            .description="Digital Ocean DDNS driver",
            .author="Kotarou",
            .version = "1.0.0"
    };
}