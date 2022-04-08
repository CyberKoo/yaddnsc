//
// Created by Kotarou on 2022/4/5.
//

#include <fmt/format.h>

#include "cloudflare.h"
#include "nlohmann/json.hpp"

constexpr char API_URL[] = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";

CloudflareDriver::CloudflareDriver() {
    required_param.emplace_back("sub_domain");
    required_param.emplace_back("zone_id");
    required_param.emplace_back("record_id");
    required_param.emplace_back("token");
    required_param.emplace_back("ip_addr");
    required_param.emplace_back("rd_type");
}

request_t CloudflareDriver::generate_request(const driver_config_t &config) {
    check_required_params(config);

    std::map<std::string, std::string> body{
            {"type",    config.at("rd_type")},
            {"name",    config.at("sub_domain")},
            {"content", config.at("ip_addr")},
            {"ttl",     config.at("$ttl")},
            {"proxied", config.at("$proxied")}
    };

    request_t request{};
    // request.header.insert({"Content-Type", "application/json"});
    request.header.insert({"Authorization", vformat("Bearer {}", {config.at("token")})});
    request.url = vformat(API_URL, {
            {"ZONE_ID",   config.at("zone_id")},
            {"RECORD_ID", config.at("record_id")}
    });
    request.body = nlohmann::json(body).dump();
    request.content_type = "application/json";
    request.request_method = request_method_t::PUT;

    return request;
}

bool CloudflareDriver::check_response(std::string_view) {
    return true;
}

driver_detail_t CloudflareDriver::get_detail() {
    return {
            .name = "cloudflare",
            .description="Cloudflare DDNS driver",
            .author="Kotarou",
            .version = "1.0.0"
    };
}
