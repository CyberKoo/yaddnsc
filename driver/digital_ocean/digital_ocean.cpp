//
// Created by Kotarou on 2022/4/5.
//
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "digital_ocean.h"

constexpr char API_URL[] = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";

DigitalOceanDriver::DigitalOceanDriver() {
    required_param_.emplace_back("domain");
    required_param_.emplace_back("record_id");
    required_param_.emplace_back("token");
    required_param_.emplace_back("ip_addr");
}

driver_request DigitalOceanDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", config.at("token"))});
    request.url = vformat(
        API_URL, {
            {"DOMAIN", config.at("domain")},
            {"RECORD_ID", config.at("record_id")}
        }
    );
    request.body = nlohmann::json({{"data", config.at("ip_addr")}}).dump();
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool DigitalOceanDriver::check_response(std::string_view response) const {
    SPDLOG_TRACE("Got {} from server.", response);
    auto json = nlohmann::json::parse(response);
    if (json.contains("domain_record")) {
        return true;
    }

    if (json.contains("message")) {
        SPDLOG_ERROR("Error from server: {}", json["message"].get<std::string>());
    } else {
        SPDLOG_ERROR("Server return an unknown error, raw response: {}", response);
    }

    return false;
}

driver_detail DigitalOceanDriver::get_detail() const {
    return {
        .name = "digital_ocean",
        .description = "Digital Ocean DDNS driver",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
